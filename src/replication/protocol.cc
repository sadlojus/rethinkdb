#include "replication/protocol.hpp"

#include "concurrency/coro_fifo.hpp"

namespace replication {
perfmon_duration_sampler_t slave_conn_reading("slave_conn_reading", secs_to_ticks(1.0));

// TODO: Do we ever really handle these?
class protocol_exc_t : public std::exception {
public:
    protocol_exc_t(const char *msg) : msg_(msg) { }
    const char *what() throw() { return msg_; }
private:
    const char *msg_;
};

// The 16 bytes conveniently include a \0 at the end.
// 13 is the length of the text.
const char STANDARD_HELLO_MAGIC[16] = "13rethinkdbrepl";

void do_parse_hello_message(tcp_conn_t *conn, message_callback_t *receiver) {
    net_hello_t buf;
    {
        block_pm_duration set_timer(&slave_conn_reading);
        conn->read(&buf, sizeof(buf));
    }

    rassert(16 == sizeof(STANDARD_HELLO_MAGIC));
    if (0 != memcmp(buf.hello_magic, STANDARD_HELLO_MAGIC, sizeof(STANDARD_HELLO_MAGIC))) {
        throw protocol_exc_t("bad hello magic");  // TODO details
    }

    if (buf.replication_protocol_version != 1) {
        throw protocol_exc_t("bad protocol version");  // TODO details
    }

    receiver->hello(buf);
}

template <class T> struct stream_type { typedef scoped_malloc<T> type; };
template <> struct stream_type<net_sarc_t> { typedef stream_pair<net_sarc_t> type; };
template <> struct stream_type<net_append_t> { typedef stream_pair<net_append_t> type; };
template <> struct stream_type<net_prepend_t> { typedef stream_pair<net_prepend_t> type; };
template <> struct stream_type<net_backfill_set_t> { typedef stream_pair<net_backfill_set_t> type; };

size_t objsize(UNUSED const net_introduce_t *buf) { return sizeof(net_introduce_t); }
size_t objsize(UNUSED const net_backfill_t *buf) { return sizeof(net_backfill_t); }
size_t objsize(UNUSED const net_backfill_complete_t *buf) { return sizeof(net_backfill_complete_t); }
size_t objsize(UNUSED const net_backfill_delete_everything_t *buf) { return sizeof(net_backfill_delete_everything_t); }
size_t objsize(UNUSED const net_nop_t *buf) { return sizeof(net_nop_t); }
size_t objsize(const net_get_cas_t *buf) { return sizeof(net_get_cas_t) + buf->key_size; }
size_t objsize(const net_incr_t *buf) { return sizeof(net_incr_t) + buf->key_size; }
size_t objsize(const net_decr_t *buf) { return sizeof(net_decr_t) + buf->key_size; }
size_t objsize(const net_delete_t *buf) { return sizeof(net_delete_t) + buf->key_size; }
size_t objsize(const net_sarc_t *buf) { return sizeof(net_sarc_t) + buf->key_size + buf->value_size; }
size_t objsize(const net_append_t *buf) { return sizeof(net_append_t) + buf->key_size + buf->value_size; }
size_t objsize(const net_prepend_t *buf) { return sizeof(net_prepend_t) + buf->key_size + buf->value_size; }
size_t objsize(const net_backfill_set_t *buf) { return sizeof(net_backfill_set_t) + buf->key_size + buf->value_size; }
size_t objsize(const net_backfill_delete_t *buf) { return sizeof(net_backfill_delete_t) + buf->key_size; }


template <class T>
void check_pass(message_callback_t *receiver, const char *buf, size_t realsize) {
    if (sizeof(T) <= realsize && objsize(reinterpret_cast<const T *>(buf)) == realsize) {
        typename stream_type<T>::type b(buf, buf + realsize);
        receiver->send(b);
    } else {
        debugf("realsize: %zu sizeof(T): %zu objsize: %zu\n", realsize, sizeof(T), objsize(reinterpret_cast<const T *>(buf)));
        throw protocol_exc_t("message wrong length for message code");
    }
}

template <class T>
void check_first_size(message_callback_t *receiver, const char *buf, size_t realsize, uint32_t ident, tracker_t& streams) {
    if (sizeof(T) >= realsize
        && sizeof(T) + reinterpret_cast<const T *>(buf)->key_size <= realsize) {

        stream_pair<T> spair(buf, buf + realsize, reinterpret_cast<const T *>(buf)->value_size);
        size_t m = realsize - sizeof(T) - reinterpret_cast<const T *>(buf)->key_size;

        void (message_callback_t::*fn)(typename stream_type<T>::type&) = &message_callback_t::send;

        if (!streams.add(ident, new tracker_obj_t(boost::bind(fn, receiver, spair), spair.stream->peek() + m, reinterpret_cast<const T *>(buf)->value_size - m))) {
            throw protocol_exc_t("reused live ident code");
        }

    } else {
        throw protocol_exc_t("message too short for message code and key size");
    }
}

namespace internal {

void handle_small_message(message_callback_t *receiver, int msgcode, const char *realbuf, size_t realsize) {
    switch (msgcode) {
    case INTRODUCE: check_pass<net_introduce_t>(receiver, realbuf, realsize); break;
    case BACKFILL: check_pass<net_backfill_t>(receiver, realbuf, realsize); break;
    case BACKFILL_COMPLETE: check_pass<net_backfill_complete_t>(receiver, realbuf, realsize); break;
    case BACKFILL_DELETE_EVERYTHING: check_pass<net_backfill_delete_everything_t>(receiver, realbuf, realsize); break;
    case NOP: check_pass<net_nop_t>(receiver, realbuf, realsize); break;
    case GET_CAS: check_pass<net_get_cas_t>(receiver, realbuf, realsize); break;
    case SARC: check_pass<net_sarc_t>(receiver, realbuf, realsize); break;
    case INCR: check_pass<net_incr_t>(receiver, realbuf, realsize); break;
    case DECR: check_pass<net_decr_t>(receiver, realbuf, realsize); break;
    case APPEND: check_pass<net_append_t>(receiver, realbuf, realsize); break;
    case PREPEND: check_pass<net_prepend_t>(receiver, realbuf, realsize); break;
    case DELETE: check_pass<net_delete_t>(receiver, realbuf, realsize); break;
    case BACKFILL_SET: check_pass<net_backfill_set_t>(receiver, realbuf, realsize); break;
    case BACKFILL_DELETE: check_pass<net_backfill_delete_t>(receiver, realbuf, realsize); break;
    default: throw protocol_exc_t("invalid message code");
    }
}

void handle_first_message(message_callback_t *receiver, int msgcode, const char *realbuf, size_t realsize, uint32_t ident, tracker_t& streams) {
    switch (msgcode) {
    case SARC: check_first_size<net_sarc_t>(receiver, realbuf, realsize, ident, streams); break;
    case APPEND: check_first_size<net_append_t>(receiver, realbuf, realsize, ident, streams); break;
    case PREPEND: check_first_size<net_prepend_t>(receiver, realbuf, realsize, ident, streams); break;
    case BACKFILL_SET: check_first_size<net_backfill_set_t>(receiver, realbuf, realsize, ident, streams); break;
    default: throw protocol_exc_t("invalid message code for multipart message");
    }
}

void handle_midlast_message(const char *realbuf, size_t realsize, uint32_t ident, tracker_t& streams) {
    tracker_obj_t *tobj = streams[ident];

    if (tobj == NULL) {
        throw protocol_exc_t("inactive stream identifier");
    }

    if (realsize > tobj->bufsize) {
        throw protocol_exc_t("buffer overflows value size");
    }
    memcpy(tobj->buf, realbuf, realsize);
    tobj->buf += realsize;
    tobj->bufsize -= realsize;
}

void handle_end_of_stream(uint32_t ident, tracker_t& streams) {
    tracker_obj_t *tobj = streams[ident];
    rassert(tobj != NULL, "this can't equal null because we must have just called handle_midlast_message");
    if (tobj->bufsize != 0) {
        throw protocol_exc_t("buffer left unfilled at LAST message");
    }
    tobj->function();
    delete tobj;
    streams.drop(ident);
}

size_t handle_message(message_callback_t *receiver, const char *buf, size_t num_read, tracker_t& streams) {
    // Returning 0 means not enough bytes; returning >0 means "I consumed <this many> bytes."

    if (num_read < sizeof(net_header_t)) {
        return 0;
    }

    const net_header_t *hdr = reinterpret_cast<const net_header_t *>(buf);
    if (hdr->message_multipart_aspect == SMALL) {
        size_t msgsize = hdr->msgsize;
        if (msgsize < sizeof(net_header_t)) {
            throw protocol_exc_t("invalid msgsize");
        }

        if (num_read < msgsize) {
            return 0;
        }

        size_t realbegin = sizeof(net_header_t);
        size_t realsize = msgsize - sizeof(net_header_t);

        handle_small_message(receiver, hdr->msgcode, buf + realbegin, realsize);

        return msgsize;
    } else {
        const net_multipart_header_t *multipart_hdr = reinterpret_cast<const net_multipart_header_t *>(buf);
        size_t msgsize = multipart_hdr->msgsize;
        if (msgsize < sizeof(net_multipart_header_t)) {
            throw protocol_exc_t("invalid msgsize");
        }

        if (num_read < msgsize) {
            return 0;
        }

        uint32_t ident = multipart_hdr->ident;
        size_t realbegin = sizeof(multipart_hdr);
        size_t realsize = msgsize - sizeof(multipart_hdr);

        if (multipart_hdr->message_multipart_aspect == FIRST) {
            handle_first_message(receiver, multipart_hdr->msgcode, buf + realbegin, realsize, ident, streams);
        } else if (multipart_hdr->message_multipart_aspect == MIDDLE || multipart_hdr->message_multipart_aspect == LAST) {
            handle_midlast_message(buf + realbegin, realsize, ident, streams);
            if (multipart_hdr->message_multipart_aspect == LAST) {
                handle_end_of_stream(ident, streams);
            }
        } else {
            throw protocol_exc_t("invalid message multipart aspect code");
        }

        return msgsize;
    }
}

void do_parse_normal_messages(tcp_conn_t *conn, message_callback_t *receiver, tracker_t& streams) {

    // This is slightly inefficient: we do excess copying since
    // handle_message is forced to accept a contiguous message, even
    // the _value_ part of the message (which could very well be
    // discontiguous and we wouldn't really care).  Worst case
    // scenario: we copy everything over the network one extra time.
    const size_t shbuf_size = 0x10000;
    scoped_malloc<char> buffer(shbuf_size);
    size_t offset = 0;
    size_t num_read = 0;

    // We break out of this loop when we get a tcp_conn_t::read_closed_exc_t.
    while (true) {
        // Try handling the message.
        size_t handled = handle_message(receiver, buffer.get() + offset, num_read, streams);
        if (handled > 0) {
            rassert(handled <= num_read);
            offset += handled;
            num_read -= handled;
        } else {
            if (offset + num_read == shbuf_size) {
                scoped_malloc<char> new_buffer(shbuf_size);
                memcpy(new_buffer.get(), buffer.get() + offset, num_read);
                offset = 0;
                buffer.swap(new_buffer);
            }

            {
                block_pm_duration set_timer(&slave_conn_reading);
                num_read += conn->read_some(buffer.get() + offset + num_read, shbuf_size - (offset + num_read));
            }
        }
    }
}


void do_parse_messages(tcp_conn_t *conn, message_callback_t *receiver) {

    try {
        do_parse_hello_message(conn, receiver);

        tracker_t streams;
        do_parse_normal_messages(conn, receiver, streams);

    } catch (tcp_conn_t::read_closed_exc_t& e) {
        // Do nothing; this was to be expected.
#ifndef NDEBUG
    } catch (protocol_exc_t& e) {
        debugf("catch 'n throwing protocol_exc_t: %s\n", e.what());
        throw;
#endif
    }

    receiver->conn_closed();
}

void parse_messages(tcp_conn_t *conn, message_callback_t *receiver) {

    coro_t::spawn(boost::bind(&internal::do_parse_messages, conn, receiver));
}

}  // namespace internal



template <class net_struct_type>
void repli_stream_t::sendobj(uint8_t msgcode, net_struct_type *msg) {

    size_t obsize = objsize(msg);

    if (obsize + sizeof(net_header_t) <= 0xFFFF) {
        net_header_t hdr;
        hdr.msgsize = sizeof(net_header_t) + obsize;
        hdr.message_multipart_aspect = SMALL;
        hdr.msgcode = msgcode;

        mutex_acquisition_t ak(&outgoing_mutex_);

        try_write(&hdr, sizeof(net_header_t));
        try_write(msg, obsize);
    } else {
        net_multipart_header_t hdr;
        hdr.msgsize = 0xFFFF;
        hdr.message_multipart_aspect = FIRST;
        hdr.ident = 1;        // TODO: This is an obvious bug.
        hdr.msgcode = msgcode;

        size_t offset = 0xFFFF - sizeof(net_multipart_header_t);

        {
            mutex_acquisition_t ak(&outgoing_mutex_);
            try_write(&hdr, sizeof(net_multipart_header_t));
            try_write(msg, offset);
        }

        char *buf = reinterpret_cast<char *>(msg);

        while (offset + 0xFFFF < obsize) {
            mutex_acquisition_t ak(&outgoing_mutex_);
            hdr.message_multipart_aspect = MIDDLE;
            try_write(&hdr, sizeof(net_multipart_header_t));
            // TODO change protocol so that 0 means 0x10000 mmkay?
            try_write(buf + offset, 0xFFFF);
            offset += 0xFFFF;
        }

        {
            rassert(obsize - offset <= 0xFFFF);
            mutex_acquisition_t ak(&outgoing_mutex_);
            hdr.message_multipart_aspect = LAST;
            try_write(&hdr, sizeof(net_multipart_header_t));
            try_write(buf + offset, obsize - offset);
        }
    }
}

template <class net_struct_type>
void repli_stream_t::sendobj(uint8_t msgcode, net_struct_type *msg, const char *key, unique_ptr_t<data_provider_t> data) {
    rassert(msg->value_size == data->get_size());

    size_t bufsize = objsize(msg);
    scoped_malloc<char> buf(bufsize);
    memcpy(buf.get(), msg, sizeof(net_struct_type));
    memcpy(buf.get() + sizeof(net_struct_type), key, msg->key_size);

    /* We guarantee that if data->get_data_into_buffers() fails, we rethrow the exception without
    sending anything over the wire. */
    buffer_group_t group;
    group.add_buffer(data->get_size(), buf.get() + sizeof(net_struct_type) + msg->key_size);

    // TODO: This could theoretically block and that could cause
    // reordering of sets.  The fact that it doesn't block is just a
    // function of whatever data provider which we happen to use.
    data->get_data_into_buffers(&group);

    sendobj(msgcode, reinterpret_cast<net_struct_type *>(buf.get()));
}

void repli_stream_t::send(net_introduce_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(INTRODUCE, msg);
}

void repli_stream_t::send(net_backfill_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(BACKFILL, msg);
}

void repli_stream_t::send(net_backfill_complete_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(BACKFILL_COMPLETE, msg);
}

void repli_stream_t::send(net_backfill_delete_everything_t msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(BACKFILL_DELETE_EVERYTHING, &msg);
}

void repli_stream_t::send(net_get_cas_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(GET_CAS, msg);
}

void repli_stream_t::send(net_sarc_t *msg, const char *key, unique_ptr_t<data_provider_t> value) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(SARC, msg, key, value);
}

void repli_stream_t::send(net_backfill_set_t *msg, const char *key, unique_ptr_t<data_provider_t> value) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(BACKFILL_SET, msg, key, value);
}

void repli_stream_t::send(net_incr_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(INCR, msg);
}

void repli_stream_t::send(net_decr_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(DECR, msg);
}

void repli_stream_t::send(net_append_t *msg, const char *key, unique_ptr_t<data_provider_t> value) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(APPEND, msg, key, value);
}

void repli_stream_t::send(net_prepend_t *msg, const char *key, unique_ptr_t<data_provider_t> value) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(PREPEND, msg, key, value);
}

void repli_stream_t::send(net_delete_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(DELETE, msg);
}

void repli_stream_t::send(net_backfill_delete_t *msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(BACKFILL_DELETE, msg);
}

void repli_stream_t::send(net_nop_t msg) {
    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);
    sendobj(NOP, &msg);
}

void repli_stream_t::send_hello(UNUSED const mutex_acquisition_t& evidence_of_acquisition) {

    drain_semaphore_t::lock_t keep_us_alive(&drain_semaphore_);

    net_hello_t msg;
    rassert(sizeof(msg.hello_magic) == 16);
    // TODO make a #define for this.
    memcpy(msg.hello_magic, "13rethinkdbrepl", 16);
    msg.replication_protocol_version = 1;

    try_write(&msg, sizeof(msg));
}

perfmon_duration_sampler_t master_write("master_write", secs_to_ticks(1.0));

void repli_stream_t::try_write(const void *data, size_t size) {
    try {
        block_pm_duration set_timer(&master_write);
        conn_->write(data, size);
    } catch (tcp_conn_t::write_closed_exc_t &e) {
        /* Master died; we happened to be mid-write at the time. A tcp_conn_t::read_closed_exc_t
        will be thrown somewhere and that will cause us to shut down. So we can ignore this
        exception. */
    }
}

}  // namespace replication
