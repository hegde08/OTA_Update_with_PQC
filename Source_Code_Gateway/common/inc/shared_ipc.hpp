/*
 * shared_ipc.hpp
 *
 *  Created on: Jul 12, 2026
 *      Author: hegde
 */

#ifndef INC_SHARED_IPC_HPP_
#define INC_SHARED_IPC_HPP_

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/trace.h>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <atomic>
#include <thread>

using namespace std;

constexpr char VERIFIER_SERVER_NAME[] = "update_verifier";
constexpr char WRITER_SERVER_NAME[]   = "update_writer";

constexpr size_t MAX_SIGNATURE_SIZE = 2420;

// QNX Kernel Trace Event Codes for System Profiler
constexpr unsigned int TRACE_EVT_OTA_START                  = 150;
constexpr unsigned int TRACE_EVT_DOWNLOAD_START             = 151;
constexpr unsigned int TRACE_EVT_DOWNLOAD_END       		= 152;
constexpr unsigned int TRACE_EVT_STREAM_DEC_START   		= 153;
constexpr unsigned int TRACE_EVT_STREAM_DEC_END     		= 154;
constexpr unsigned int TRACE_EVT_SIG_VERIFY_START   		= 155;
constexpr unsigned int TRACE_EVT_SIG_VERIFY_END     		= 156;
constexpr unsigned int TRACE_EVT_FLASH_GATEWAY_START        = 157;
constexpr unsigned int TRACE_EVT_FLASH_GATEWAY_END          = 158;
constexpr unsigned int TRACE_EVT_FLASH_END_ECU_START        = 159;
constexpr unsigned int TRACE_EVT_FLASH_END_ECU_END          = 160;
constexpr unsigned int TRACE_EVT_OTA_COMPLETE       		= 161;

enum SigAlgo : uint8_t
{
    SIG_ALGO_ECDSA      = 0,
    SIG_ALGO_FALCON512  = 1,
    SIG_ALGO_MLDSA44    = 2
};

enum MsgType : uint16_t
{
    MSG_VERIFY_FILE          = _IO_MAX + 1,
    MSG_VERIFY_FILE_ZEROCOPY = _IO_MAX + 2,
    MSG_WRITE_PARTITION      = _IO_MAX + 3,
    MSG_WRITE_STM32_UART     = _IO_MAX + 4,
    MSG_ABORT_OP             = _IO_MAX + 5
};

#pragma pack(push, 1)
struct OtaHeader {
    uint32_t magic;
    uint32_t headerSize;
    uint64_t payloadSize;
    uint8_t  updateType;
    uint8_t  sigAlgo;
    uint32_t sigLen;
    uint8_t  version[16];
    uint8_t  iv[12];
    uint8_t  gcmTag[16];
    uint8_t  signature[MAX_SIGNATURE_SIZE];
};

struct ExecutionMetrics {
    uint64_t streamDecryptMs;
    uint64_t signatureVerifyUs;
    uint32_t signatureSize;         // Exact signature byte size for PQC analysis
    uint32_t publicKeySize;         // Public key size in bytes
    double   pqcOverheadPct;        // Signature overhead relative to OtaHeader size
    uint64_t extractionMs;
    uint64_t flashDurationMs;
    double   flashThroughputMBs;
};

// Zero-copy update message containing direct payload offset
struct UpdateMsg
{
    uint16_t  type;
    char      filepath[256];
    uint32_t  payloadOffset; // Offset where payload starts (0 if pre-sliced)
    OtaHeader header;
};

struct UpdateReply
{
    int status;
    ExecutionMetrics metrics;
};
#pragma pack(pop)

class IpcServer
{
protected:
    name_attach_t* attach;
    string serverName;

    void sendReply(int rcvid, int status, const ExecutionMetrics& metrics = {})
    {
        UpdateReply reply{};
        reply.status = status;
        reply.metrics = metrics;
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }

public:
    IpcServer(const string &name) : serverName(name)
    {
        attach = name_attach(nullptr, serverName.c_str(), 0);
        if(!attach)
        {
            throw runtime_error("Failed to attach name: " + serverName);
        }
    }

    virtual ~IpcServer()
    {
        if(attach)
        {
            name_detach(attach, 0);
        }
    }

    virtual void processMessage(int rcvid, const UpdateMsg& msg) = 0;
    virtual void onAbortRequested() {}

    void run()
    {
        UpdateMsg Msg;
        struct _msg_info info;

        cout << "[" << serverName << "] Started. Listening...\n";

        while(true)
        {
            int rcvid = MsgReceive(attach->chid, &Msg, sizeof(Msg), &info);
            if(rcvid == -1)
            {
                break;
            }
            if(rcvid == 0)
            {
                struct _pulse* pulse = reinterpret_cast<struct _pulse*>(&Msg);
                if (pulse->code == _PULSE_CODE_DISCONNECT || pulse->code == _PULSE_CODE_UNBLOCK) {
                    onAbortRequested();
                }
                continue;
            }

            if(Msg.type == _IO_CONNECT)
            {
                MsgReply(rcvid, EOK, nullptr, 0);
                continue;
            }

            if(Msg.type == _IO_CLOSE) {
                MsgReply(rcvid, EOK, nullptr, 0);
                continue;
            }

            if(Msg.type == MSG_ABORT_OP) {
                cout << "[" << serverName << "] Immediate MSG_ABORT_OP received!" << endl;
                onAbortRequested();
                sendReply(rcvid, 0);
                continue;
            }

            std::thread([this, rcvid, Msg]() {
                this->processMessage(rcvid, Msg);
            }).detach();
        }
    }
};

class IpcClient
{
private:
    int coid;
    string serverName;

public:
    IpcClient(const string& name) : coid(-1), serverName(name)
    {
        coid = name_open(serverName.c_str(), 0);
        if(coid == -1)
        {
            throw runtime_error("Failed to connect to server = " + serverName);
        }
    }

    ~IpcClient()
    {
        close();
    }

    void close()
    {
        if(coid != -1)
        {
            name_close(coid);
            coid = -1;
        }
    }

    // Standard MsgSend IPC
    bool send_command(uint16_t commandType, const string& filepath, const OtaHeader* header = nullptr, UpdateReply* outReply = nullptr)
    {
        if (coid == -1) return false;

        UpdateMsg msg = {};
        msg.type = commandType;
        strncpy(msg.filepath, filepath.c_str(), sizeof(msg.filepath) - 1);
        msg.payloadOffset = 0;

        if (header != nullptr) {
            msg.header = *header;
        }

        UpdateReply reply{};
        if(MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) == -1)
        {
            cerr << "Msg send failed to server: " << serverName << " (peer may have crashed)" << endl;
            return false;
        }

        if (outReply != nullptr) {
            *outReply = reply;
        }
        return (reply.status == 0);
    }

    // Zero-Copy Scatter-Gather Vector IPC (MsgSendv)
    bool send_command_iov(uint16_t commandType, const string& filepath, uint32_t payloadOffset, const OtaHeader* header, UpdateReply* outReply = nullptr)
    {
        if (coid == -1) return false;

        iov_t s_iov[4];
        uint16_t type = commandType;
        char pathBuf[256] = {0};
        strncpy(pathBuf, filepath.c_str(), sizeof(pathBuf) - 1);

        SETIOV(&s_iov[0], &type, sizeof(type));
        SETIOV(&s_iov[1], pathBuf, sizeof(pathBuf));
        SETIOV(&s_iov[2], &payloadOffset, sizeof(payloadOffset));

        static const OtaHeader dummyHeader{};
        const OtaHeader* hPtr = (header != nullptr) ? header : &dummyHeader;
        SETIOV(&s_iov[3], const_cast<OtaHeader*>(hPtr), sizeof(OtaHeader));

        UpdateReply reply{};
        iov_t r_iov;
        SETIOV(&r_iov, &reply, sizeof(reply));

        if (MsgSendv(coid, s_iov, 4, &r_iov, 1) == -1) {
            cerr << "MsgSendv failed to server: " << serverName << endl;
            return false;
        }

        if (outReply != nullptr) {
            *outReply = reply;
        }
        return (reply.status == 0);
    }
};
#endif /* INC_SHARED_IPC_HPP_ */
