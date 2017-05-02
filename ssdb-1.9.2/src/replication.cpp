//
// Created by zts on 17-4-27.
//
#include "replication.h"
#include <util/thread.h>
#include <net/link.h>
#include <redis/rdb.h>
#include "serv.h"


void send_error_to_redis(Link *link);

int ssdb_save_len(uint64_t len, std::string &res);

void moveBuffer(Buffer *dst, Buffer *src);

void saveStrToBuffer(Buffer *buffer, const Bytes &fit);

ReplicationWorker::ReplicationWorker(const std::string &name) {
    this->name = name;
}

ReplicationWorker::~ReplicationWorker() {

}

void ReplicationWorker::init() {
    log_debug("%s %d init", this->name.c_str(), this->id);
}

int ReplicationWorker::proc(ReplicationJob *job) {

    SSDBServer *serv = job->serv;
    HostAndPort hnp = job->hnp;
    Link *master_link = job->upstreamRedis;
    const leveldb::Snapshot *snapshot = nullptr;

    log_info("[ReplicationWorker] send snapshot to %s:%d start!", hnp.ip.c_str(), hnp.port);
    {
        Locking<Mutex> l(&serv->replicMutex);
        if (serv->replicSnapshot == nullptr) {
            log_error("snapshot is null, maybe rr_make_snapshot not receive or error!");
            send_error_to_redis(master_link);
            return 0;
        }
        snapshot = serv->replicSnapshot;
    }
    std::unique_ptr<Iterator> fit = std::unique_ptr<Iterator>(serv->ssdb->iterator("", "", -1, snapshot));

    Link *ssdb_slave_link = Link::connect((hnp.ip).c_str(), hnp.port);
    if (ssdb_slave_link == nullptr) {
        log_error("fail to connect to slave ssdb! ip[%s] port[%d]", hnp.ip.c_str(), hnp.port);
        log_debug("replic send snapshot failed!");
        send_error_to_redis(master_link);
        {
            Locking<Mutex> l(&serv->replicMutex);
            serv->replicNumFailed++;
            if (serv->replicNumFinished == (serv->replicNumStarted + serv->replicNumFailed))
                serv->replicState = REPLIC_END;
        }
        return 0;
    }
    ssdb_slave_link->send(std::vector<std::string>({"sync150"}));
    ssdb_slave_link->write();
    ssdb_slave_link->response();
    ssdb_slave_link->noblock(true);


    log_debug("[ReplicationWorker] prepare for event loop");
    unique_ptr<Fdevents> fdes = unique_ptr<Fdevents>(new Fdevents());
    fdes->set(master_link->fd(), FDEVENT_IN, 1, master_link); //open evin
    const Fdevents::events_t *events;
    ready_list_t ready_list;
    ready_list_t ready_list_2;
    ready_list_t::iterator it;

    std::unique_ptr<Buffer> buffer = std::unique_ptr<Buffer>(new Buffer(8 * 1024));

    while (!job->quit) {
        ready_list.swap(ready_list_2);
        ready_list_2.clear();

        if (!ready_list.empty()) {
            // ready_list not empty, so we should return immediately
            events = fdes->wait(0);
        } else {
            events = fdes->wait(5);
        }

        if (events == nullptr) {
            log_fatal("events.wait error: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < (int) events->size(); i++) {
            //processing
            const Fdevent *fde = events->at(i);
            Link *link = (Link *) fde->data.ptr;
            if (fde->events & FDEVENT_IN) {
                ready_list.push_back(link);
                if (link->error()) {
                    continue;
                }
                int len = link->read();
                if (len <= 0) {
                    log_debug("fd: %d, read: %d, delete link, e:%d, f:%d", link->fd(), len, fde->events, fde->s_flags);
                    link->mark_error();
                    continue;
                }
            }
            if (fde->events & FDEVENT_OUT) {
                ready_list.push_back(link); //push into ready_list
                if (link->error()) {
                    continue;
                }
                int len = link->write();
                if (len <= 0) {
                    log_debug("fd: %d, write: %d, delete link, e:%d, f:%d", link->fd(), len, fde->events, fde->s_flags);
                    link->mark_error();
                    continue;
                }
                if (link->output->empty()) {
                    fdes->clr(link->fd(), FDEVENT_OUT);
                }
            }
        }

        for (it = ready_list.begin(); it != ready_list.end(); it++) {
            Link *link = *it;
            if (link->error()) {

                if (link == master_link) {
                    log_info("link to redis broken");
                    //TODO
                    fdes->del(ssdb_slave_link->fd());
                    fdes->del(master_link->fd());
                    delete ssdb_slave_link;
                    delete master_link;

                } else if (link == ssdb_slave_link) {
                    log_info("link to slave ssdb broken");
                    //TODO

                    master_link->noblock(false);
                    send_error_to_redis(master_link);

                    fdes->del(ssdb_slave_link->fd());
                    fdes->del(master_link->fd());
                    delete ssdb_slave_link;
                    delete master_link;

                }

                {
                    Locking<Mutex> l(&serv->replicMutex);
                    serv->replicNumFinished++;
                    if (serv->replicNumFinished == (serv->replicNumStarted + serv->replicNumFailed)) {
                        serv->replicState = REPLIC_END;
                    }
                }

                return -1;
//                fdes->del(link->fd());
//                delete link;
//                continue;
            }
        }

        if (ssdb_slave_link->output->size() > (1024 * 1024)) {
            log_debug("delay for output buffer write slow~");
            continue;
        }

        bool finish = true;
        while (fit->next()) {
            saveStrToBuffer(buffer.get(), fit->key());
            saveStrToBuffer(buffer.get(), fit->val());

            if (buffer->size() > (10 * 1024)) {
                saveStrToBuffer(ssdb_slave_link->output, "mset");
                moveBuffer(ssdb_slave_link->output, buffer.get());
                ssdb_slave_link->write();
                if(!ssdb_slave_link->output->empty()){
                    fdes->set(ssdb_slave_link->fd(), FDEVENT_OUT, 1, ssdb_slave_link);
                }
                finish = false;
                break;
            }
        }

        if (finish) {
            if (buffer->size() > 0) {
                saveStrToBuffer(ssdb_slave_link->output, "mset");
                moveBuffer(ssdb_slave_link->output, buffer.get());
                ssdb_slave_link->write();
                if(!ssdb_slave_link->output->empty()){
                    fdes->set(ssdb_slave_link->fd(), FDEVENT_OUT, 1, ssdb_slave_link);
                }
            }

            if (!ssdb_slave_link->output->empty()) {
                log_debug("wait for output buffer empty~");
                continue; //wait for buffer empty
            } else {
                break;
            }
        }
    }


    ssdb_slave_link->noblock(false);
    saveStrToBuffer(ssdb_slave_link->output, "complete");
    ssdb_slave_link->write();
    ssdb_slave_link->read();
    delete ssdb_slave_link;

    {
        Locking<Mutex> l(&serv->replicMutex);
        serv->replicNumFinished++;
        if (serv->replicNumFinished == (serv->replicNumStarted + serv->replicNumFailed)) {
            serv->replicState = REPLIC_END;
        }
    }

    log_info("[ReplicationWorker] send snapshot to %s:%d finished!", hnp.ip.c_str(), hnp.port);
    log_debug("send rr_transfer_snapshot finished!!");
    log_error("replic procedure finish!");

    return 0;
}


void send_error_to_redis(Link *link) {
    if (link != NULL) {
        link->send(std::vector<std::string>({"error", "rr_transfer_snapshot error"}));
        if (link->append_reply) {
            link->send_append_res(std::vector<std::string>({"check 0"}));
        }
        link->write();
        log_error("send rr_transfer_snapshot error!!");
    }
}

int ssdb_save_len(uint64_t len, std::string &res) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1 << 6)) {
        /* Save a 6 bit len */
        buf[0] = (len & 0xFF) | (RDB_6BITLEN << 6);
        res.append(1, buf[0]);
        nwritten = 1;
    } else if (len < (1 << 14)) {
        /* Save a 14 bit len */
        buf[0] = ((len >> 8) & 0xFF) | (RDB_14BITLEN << 6);
        buf[1] = len & 0xFF;
        res.append(1, buf[0]);
        res.append(1, buf[1]);
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        res.append(1, buf[0]);
        uint32_t len32 = htobe32(len);
        res.append((char *) &len32, sizeof(uint32_t));
        nwritten = 1 + 4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        res.append(1, buf[0]);
        len = htobe64(len);
        res.append((char *) &len, sizeof(uint64_t));
        nwritten = 1 + 8;
    }
    return (int) nwritten;
}


void saveStrToBuffer(Buffer *buffer, const Bytes &fit) {
    string val_len;
    ssdb_save_len((uint64_t) (fit.size()), val_len);
    buffer->append(val_len);
    buffer->append(fit);
}

void moveBuffer(Buffer *dst, Buffer *src) {
    char buf[32] = {0};
    ll2string(buf, sizeof(buf) - 1, (long long) src->size());
    string buffer_size(buf);
    string buffer_len;
    ssdb_save_len((uint64_t) buffer_size.size(), buffer_len);
    dst->append(buffer_len);
    dst->append(buffer_size);

    dst->append(src->data(), src->size());

    src->decr(src->size());
    src->nice();
}