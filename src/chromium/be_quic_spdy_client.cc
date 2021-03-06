#include "net/tools/quic/be_quic_spdy_client.h"
#include "net/tools/quic/be_quic_spdy_client_session.h"
#include "net/tools/quic/be_quic_define.h"
#include "net/tools/quic/be_quic_client_message_loop_network_helper.h"

#include "base/logging.h"
#include "base/run_loop.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_info.h"
#include "net/log/net_log_source.h"
#include "net/log/net_log_with_source.h"
#include "net/quic/quic_chromium_alarm_factory.h"
#include "net/quic/quic_chromium_connection_helper.h"
#include "net/quic/quic_chromium_packet_reader.h"
#include "net/quic/quic_chromium_packet_writer.h"
#include "net/socket/udp_client_socket.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/third_party/quiche/src/quic/core/crypto/quic_random.h"
#include "net/third_party/quiche/src/quic/core/http/spdy_utils.h"
#include "net/third_party/quiche/src/quic/core/quic_connection.h"
#include "net/third_party/quiche/src/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quic/core/quic_server_id.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_flags.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_ptr_util.h"
#include "net/third_party/quiche/src/spdy/core/spdy_header_block.h"

#include <utility>

#define AVSEEK_SIZE     0x10000
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

namespace net {

const int kReadBlockSize = 32768;

BeQuicSpdyClient::BeQuicSpdyClient(
    quic::QuicSocketAddress server_address,
    const quic::QuicServerId& server_id,
    const quic::ParsedQuicVersionVector& supported_versions,
    std::unique_ptr<quic::ProofVerifier> proof_verifier)
    : quic::QuicSpdyClientBase(
        server_id,
        supported_versions,
        quic::QuicConfig(),
        CreateQuicConnectionHelper(),
        CreateQuicAlarmFactory(),
        quic::QuicWrapUnique(new BeQuicClientMessageLooplNetworkHelper(&clock_, this)),
        std::move(proof_verifier)),
      istream_(&response_buff_),
      ostream_(&response_buff_),
      weak_factory_(this) {
    set_server_address(server_address);
}

BeQuicSpdyClient::~BeQuicSpdyClient() {
    if (connected()) {
        session()->connection()->CloseConnection(
            quic::QUIC_PEER_GOING_AWAY,
            "Shutting down",
            quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
    }
}

QuicChromiumConnectionHelper* BeQuicSpdyClient::CreateQuicConnectionHelper() {
    return new QuicChromiumConnectionHelper(&clock_, quic::QuicRandom::GetInstance());
}

QuicChromiumAlarmFactory* BeQuicSpdyClient::CreateQuicAlarmFactory() {
    return new QuicChromiumAlarmFactory(base::ThreadTaskRunnerHandle::Get().get(), &clock_);
}

std::unique_ptr<quic::QuicSession> BeQuicSpdyClient::CreateQuicClientSession(
        const quic::ParsedQuicVersionVector& supported_versions,
        quic::QuicConnection* connection) {
    std::unique_ptr<quic::BeQuicSpdyClientSession> session = quic::QuicMakeUnique<quic::BeQuicSpdyClientSession>(
        *config(),
        supported_versions,
        connection,
        server_id(),
        crypto_config(),
        push_promise_index());
    session.get()->set_delegate(shared_from_this());
    return session;
}

int BeQuicSpdyClient::read_body(unsigned char *buf, int size, int timeout) {
    int ret = 0;
    do {
        if (buf == NULL || size == 0) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        //TBD:Chunk?
        if (content_length_ > 0 && read_offset_ >= content_length_) {
            ret = kBeQuicErrorCode_Eof;
            break;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        while (!is_buffer_sufficient()) {
            if (timeout > 0) {
                //Wait for certain time.
                //LOG(INFO) << "buf size 0 will wait " << timeout << "ms" << std::endl;
                cond_.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeout));
            } else if (timeout < 0) {
                //Wait forever.
                cond_.wait(lock);
            }
            break;
        }
        size_t read_len = std::min<size_t>((size_t)size, response_buff_.size());
        if (read_len == 0) {
            break;
        }

        istream_.read((char*)buf, read_len);
        read_offset_ += read_len;
        ret = (int)read_len;
        //LOG(INFO) << "buf read " << ret << "byte" << std::endl;
    } while (0);
    return ret;
}

int64_t BeQuicSpdyClient::seek_in_buffer(int64_t off, int whence, int64_t *target_off) {
    //Since calling from worker thread, lock is unnecessary.
    //std::unique_lock<std::mutex> lock(mutex_);
    int64_t ret = -1;
    do {
        if (content_length_ == -1) {
            ret = kBeQuicErrorCode_Not_Supported;
            break;
        }

        if (whence == AVSEEK_SIZE) {
            ret = content_length_;
            break;
        }

        if ((whence == SEEK_CUR && off == 0) || (whence == SEEK_SET && off == read_offset_)) {
            ret = off;
            break;
        }

        if (content_length_ == -1 && whence == SEEK_END) {
            ret = kBeQuicErrorCode_Invalid_State;
            break;
        }

        if (whence == SEEK_CUR) {
            off += read_offset_;
        } else if (whence == SEEK_END) {
            off += content_length_;
        } else if (whence != SEEK_SET) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        if (off < 0) {
            ret = kBeQuicErrorCode_Invalid_Param;
            break;
        }

        //Check if hit the buffer.
        int64_t left_size = (int64_t)response_buff_.size();
        int64_t consume_size = off - read_offset_;

        if (consume_size > 0 && left_size > consume_size) {
            response_buff_.consume(consume_size);
            read_offset_ = off;
            ret = off;
            break;
        }

        if (target_off != NULL) {
            *target_off = off;
        }

        ret = kBeQuicErrorCode_Buffer_Not_Hit;
    } while (0);

    LOG(INFO) << "seek_in_buffer " << off << " " << whence << " return "  << ret << std::endl;
    return ret;
}

bool BeQuicSpdyClient::close_current_stream() {
    bool ret = true;
    do {
        if (current_stream_id_ == 0) {
            ret = false;
            break;
        }

        quic::QuicSession *session = QuicClientBase::session();
        if (session == NULL) {
            ret = false;
            break;
        }

        LOG(INFO) << "Closing stream " << current_stream_id_ << std::endl;

        //Close quic stream, send Reset frame to close peer stream.
        session->SendRstStream(current_stream_id_, quic::QUIC_STREAM_CANCELLED, 0);
        session->CloseStream(current_stream_id_);

        current_stream_id_ = 0;
        read_offset_ = 0;

        //Clear cached data.
        response_buff_.consume(response_buff_.size());
    } while (0);
    return ret;
}

void BeQuicSpdyClient::on_data(quic::QuicSpdyClientStream *stream, char *buf, int size) {
    if (stream == NULL) {
        return;
    }

    if (current_stream_id_ == 0) {
        current_stream_id_ = stream->id();
        LOG(INFO) << "Bound to stream " << current_stream_id_ << std::endl;
    }

    if (!got_first_data_) {
        quic::BeQuicSpdyClientStream* bequic_stream = static_cast<quic::BeQuicSpdyClientStream*>(stream);
        content_length_     = bequic_stream->check_content_length();
        first_data_time_    = base::Time::Now();
        got_first_data_     = true;
    }

    if (buf != NULL && size > 0) {
        std::unique_lock<std::mutex> lock(mutex_);
        ostream_.write(buf ,size);
        if (is_buffer_sufficient()) {
            //LOG(INFO) << "buf write one block " << response_buff_.size() << std::endl;
            cond_.notify_all();
        }
    }
}

bool BeQuicSpdyClient::is_buffer_sufficient() {
    bool ret = true;
    do {
        size_t size = response_buff_.size();
        if (content_length_ == -1) {
            //Cannot determine end of stream, so if some data exists just return true for safe.
            ret = size > 0;
            break;
        }

        if (size == 0) {
            ret = false;
            break;
        }

        if (content_length_ - read_offset_ < kReadBlockSize) {
            ret = true;
            break;
        }

        if (size < kReadBlockSize) {
            ret = false;
            break;
        }

        ret = true;
    } while (0);
    return ret;
}

}  // namespace net
