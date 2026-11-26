#include "download_agent.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <csignal>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

// Set to true to test the Zero-Copy IOV & In-Place Pipeline; false for Baseline
constexpr bool ENABLE_ZERO_COPY_PIPELINE = true;

SecureDownloader::SecureDownloader()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        throw runtime_error("Failed to init CURL");
    }
}

SecureDownloader::~SecureDownloader()
{
    if (curl) curl_easy_cleanup(curl);
    curl_global_cleanup();
}

void notifyAbortAndExit(int sig) {
    std::cerr << "\n[SecureDownloader] Interrupted (" << sig << ")! Aborting Writer & Verifier..." << std::endl;

    int writer_coid = name_open(WRITER_SERVER_NAME, 0);
    if (writer_coid != -1) {
        UpdateMsg msg = {};
        msg.type = MSG_ABORT_OP;
        UpdateReply reply;
        MsgSend(writer_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        name_close(writer_coid);
    }

    int verifier_coid = name_open(VERIFIER_SERVER_NAME, 0);
    if (verifier_coid != -1) {
        UpdateMsg msg = {};
        msg.type = MSG_ABORT_OP;
        UpdateReply reply;
        MsgSend(verifier_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        name_close(verifier_coid);
    }

    unlink("/dev/shmem/ota_package.bin");
    unlink("/dev/shmem/ota_package.tar.gz");
    unlink("/dev/shmem/ifs-rpi4.ui");
    unlink("/dev/shmem/rootfs.img");
    unlink("/dev/shmem/Application_OTA_Encrypted.bin");
    unlink("/dev/shmem/Application.sig");

    _exit(128 + sig);
}

std::vector<int> SecureDownloader::parseVersionTokens(const std::string& verStr) const {
    std::vector<int> tokens;
    std::stringstream ss(verStr);
    std::string token;
    while (std::getline(ss, token, '.')) {
        try {
            tokens.push_back(std::stoi(token));
        } catch (...) {
            tokens.push_back(0);
        }
    }
    return tokens;
}

int SecureDownloader::compareVersions(const std::string& incomingVer, const std::string& currentVer) const {
    auto v1 = parseVersionTokens(incomingVer);
    auto v2 = parseVersionTokens(currentVer);

    size_t maxLen = std::max(v1.size(), v2.size());
    for (size_t i = 0; i < maxLen; ++i) {
        int seg1 = (i < v1.size()) ? v1[i] : 0;
        int seg2 = (i < v2.size()) ? v2[i] : 0;
        if (seg1 > seg2) return 1;
        if (seg1 < seg2) return -1;
    }
    return 0;
}

bool SecureDownloader::readCurrentVersion(const std::string& verFilePath, std::string& outVersion) const {
    std::ifstream inFile(verFilePath);
    if (!inFile.is_open()) {
        std::cerr << "[SecureDownloader] Version file not found at " << verFilePath << std::endl;
        return false;
    }
    std::getline(inFile, outVersion);
    outVersion.erase(outVersion.find_last_not_of(" \n\r\t") + 1);
    return !outVersion.empty();
}

bool SecureDownloader::validateVersion(const OtaHeader& header,
                                     const std::string& verFilePath,
                                     std::string& outIncomingVersion) {
    outIncomingVersion.assign(
        reinterpret_cast<const char*>(header.version),
        strnlen(reinterpret_cast<const char*>(header.version), sizeof(header.version))
    );

    std::string currentVersion;
    if (!readCurrentVersion(verFilePath, currentVersion)) {
        std::cerr << "[SecureDownloader] Flashing aborted: Cannot read current version." << std::endl;
        return false;
    }

    std::cout << "[SecureDownloader] Current: " << currentVersion << " | Incoming: " << outIncomingVersion << std::endl;
    return (compareVersions(outIncomingVersion, currentVersion) > 0);
}

// Baseline file slicer
bool SecureDownloader::stripHeaderAndExtractZip(const string& downloadedFile, 
                              const string& outZipFile, 
                              OtaHeader& outHeader,
                              uint64_t& sliceDurationMs)
{
    auto tStart = chrono::steady_clock::now();
    ifstream inFile(downloadedFile, ios::binary);
    if (!inFile) return false;

    inFile.read(reinterpret_cast<char*>(&outHeader), sizeof(OtaHeader));
    if (outHeader.magic != 0x4F544131) {
        cerr << "[SecureDownloader] Invalid header magic." << endl;
        return false;
    }

    ofstream zipFile(outZipFile, ios::binary);
    if (!zipFile) return false;

    zipFile << inFile.rdbuf();

    inFile.close();
    zipFile.close();

    auto tEnd = chrono::steady_clock::now();
    sliceDurationMs = chrono::duration_cast<chrono::milliseconds>(tEnd - tStart).count();
    return true;
}

// Zero-copy in-place header parser
bool SecureDownloader::parseHeaderInPlace(const string& downloadedFile,
                                         OtaHeader& outHeader,
                                         uint32_t& outPayloadOffset)
{
    int fd = open(downloadedFile.c_str(), O_RDONLY);
    if (fd < 0) return false;

    ssize_t bytes = read(fd, &outHeader, sizeof(OtaHeader));
    close(fd);

    if (bytes != sizeof(OtaHeader) || outHeader.magic != 0x4F544131) {
        cerr << "[SecureDownloader] Zero-copy parsing failed: invalid header or magic." << endl;
        return false;
    }

    outPayloadOffset = sizeof(OtaHeader);
    return true;
}

size_t SecureDownloader::writeCallback(void* contents, size_t size, size_t nmemb, FILE* stream)
{
	return fwrite(contents, size, nmemb, stream);
}

bool SecureDownloader::download(const string& url, const string& outPath,
                  const string& cert, const string& key, const string& ca,
                  DownloadMetrics& metrics)
{
    FILE* fp = fopen(outPath.c_str(), "wb");
    if (!fp) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    curl_easy_setopt(curl, CURLOPT_SSLCERT, cert.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLKEY, key.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);

    if (res != CURLE_OK) return false;

    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &metrics.totalTimeSec);
    curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, &metrics.speedDownloadBytesSec);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &metrics.downloadedBytes);

    return true;
}

static void logBenchmarkRecord(
    const string& mode,
    const string& algoName,
    const DownloadMetrics& dl,
    uint64_t sliceMs,
    const ExecutionMetrics& verifierMetrics,
    const ExecutionMetrics& writerMetrics,
    uint64_t totalDurationMs)
{
    mkdir("/fs/var", 0755);
    mkdir("/fs/var/log", 0755);

    const string csvPath = "/fs/var/log/ota_benchmark.csv";
    bool fileExists = (access(csvPath.c_str(), F_OK) == 0);

    ofstream csv(csvPath, ios::app);
    if (!csv.is_open()) return;

    if (!fileExists) {
        csv << "Timestamp,Mode,Algorithm,SigLen_Bytes,PubKeyLen_Bytes,HeaderOverhead_Pct,"
            << "SigVerify_us,DownloadSize_Bytes,DownloadTime_s,DownloadSpeed_MBps,"
            << "SliceTime_ms,StreamDecrypt_ms,TarExtract_ms,FlashDuration_ms,"
            << "FlashSpeed_MBps,TotalPipeline_ms,SigVerify_Pct_Downtime\n";
    }

    double sigDowntimePct = (totalDurationMs > 0)
        ? ((verifierMetrics.signatureVerifyUs / 1000.0) / totalDurationMs) * 100.0
        : 0.0;

    auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    csv << now << ","
        << mode << ","
        << algoName << ","
        << verifierMetrics.signatureSize << ","
        << verifierMetrics.publicKeySize << ","
        << fixed << setprecision(2) << verifierMetrics.pqcOverheadPct << ","
        << verifierMetrics.signatureVerifyUs << ","
        << dl.downloadedBytes << ","
        << setprecision(3) << dl.totalTimeSec << ","
        << setprecision(2) << (static_cast<double>(dl.speedDownloadBytesSec) / (1024.0 * 1024.0)) << ","
        << sliceMs << ","
        << verifierMetrics.streamDecryptMs << ","
        << writerMetrics.extractionMs << ","
        << writerMetrics.flashDurationMs << ","
        << writerMetrics.flashThroughputMBs << ","
        << totalDurationMs << ","
        << setprecision(4) << sigDowntimePct << "\n";

    csv.close();
}

int main() {
	signal(SIGINT, notifyAbortAndExit);
	signal(SIGTERM, notifyAbortAndExit);
	signal(SIGHUP, notifyAbortAndExit);

    const string rawDownloadPath = "/dev/shmem/ota_package.bin";
    const string zipFilePath     = "/dev/shmem/ota_package.tar.gz";

    auto pipelineStart = chrono::steady_clock::now();
    TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_OTA_START, "OTA_START");

    try {
        cout << "[SecureDownloader] Initiating mTLS download..." << endl;
        SecureDownloader downloader;
        DownloadMetrics dlMetrics{};

        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_DOWNLOAD_START, "DOWNLOAD_START");
        bool dlSuccess = downloader.download(
            "https://192.168.1.88:4433/ota_package.bin", rawDownloadPath,
            "/fs/var/certs/ecu.crt", "/fs/var/certs/ecu.key", "/fs/var/certs/rootCA.crt",
            dlMetrics
        );
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_DOWNLOAD_END, "DOWNLOAD_END");

        if (!dlSuccess)
        {
            cerr << "[SecureDownloader] Download failed!" << endl;
            unlink(rawDownloadPath.c_str());
            return 1;
        }

        cout << fixed << setprecision(2);
        cout << "[SecureDownloader] Download Stats: "
             << (static_cast<double>(dlMetrics.downloadedBytes) / (1024.0 * 1024.0)) << " MB in "
             << dlMetrics.totalTimeSec << " s ("
             << (static_cast<double>(dlMetrics.speedDownloadBytesSec) / (1024.0 * 1024.0)) << " MB/s)" << endl;

        OtaHeader parsedHeader{};
        uint64_t sliceMs = 0;
        uint32_t payloadOffset = 0;
        string verificationTarget;

        if (ENABLE_ZERO_COPY_PIPELINE)
        {
            cout << "[SecureDownloader] MODE: Zero-Copy IOV & In-Place Offset Enabled." << endl;
            auto t0 = chrono::steady_clock::now();
            if (!downloader.parseHeaderInPlace(rawDownloadPath, parsedHeader, payloadOffset))
            {
                unlink(rawDownloadPath.c_str());
                return 1;
            }
            auto t1 = chrono::steady_clock::now();
            sliceMs = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
            verificationTarget = rawDownloadPath;
        }
        else
        {
            cout << "[SecureDownloader] MODE: Baseline Slicing Enabled." << endl;
            if (!downloader.stripHeaderAndExtractZip(rawDownloadPath, zipFilePath, parsedHeader, sliceMs))
            {
                unlink(rawDownloadPath.c_str());
                unlink(zipFilePath.c_str());
                return 1;
            }
            unlink(rawDownloadPath.c_str());
            verificationTarget = zipFilePath;
            payloadOffset = 0;
        }

        std::string verPath = (parsedHeader.updateType == 1)
                              ? "/fs/var/version/gateway_version"
                              : "/fs/var/version/end_ecu_version";

        std::string validatedVersion;
        if (!downloader.validateVersion(parsedHeader, verPath, validatedVersion))
        {
			unlink(verificationTarget.c_str());
			cout<<"[SecureDownloader] Version is same or lesser than Current version, Flashing aborted."<<endl;
			return 1;
        }

        cout << "[SecureDownloader] Connecting to Verifier..." << endl;
        IpcClient verifierClient(VERIFIER_SERVER_NAME);
        UpdateReply verifierReply{};
        bool vSuccess = false;

        if (ENABLE_ZERO_COPY_PIPELINE) {
            vSuccess = verifierClient.send_command_iov(
                MSG_VERIFY_FILE_ZEROCOPY,
                verificationTarget,
                payloadOffset,
                &parsedHeader,
                &verifierReply
            );
        } else {
            vSuccess = verifierClient.send_command(
                MSG_VERIFY_FILE,
                verificationTarget,
                &parsedHeader,
                &verifierReply
            );
        }

        verifierClient.close();

        if (!vSuccess)
        {
            cerr << "[SecureDownloader] Verification rejected." << endl;
            unlink(verificationTarget.c_str());
            return 1;
        }

        cout << "[SecureDownloader] Connecting to Writer..." << endl;
        IpcClient writerClient(WRITER_SERVER_NAME);
        UpdateReply writerReply{};
        MsgType UpdateType = (parsedHeader.updateType == 1) ? MSG_WRITE_PARTITION : MSG_WRITE_STM32_UART;

        bool wSuccess = writerClient.send_command(UpdateType, zipFilePath, nullptr, &writerReply);
        writerClient.close();

        if (!wSuccess)
        {
            cerr << "[SecureDownloader] Flash write failed." << endl;
            return 1;
        }

        auto pipelineEnd = chrono::steady_clock::now();
        TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_OTA_COMPLETE, "OTA_COMPLETE");

        uint64_t totalDurationMs = chrono::duration_cast<chrono::milliseconds>(pipelineEnd - pipelineStart).count();

        string algoName = (parsedHeader.sigAlgo == SIG_ALGO_ECDSA) ? "ECDSA_secp256r1" :
                          (parsedHeader.sigAlgo == SIG_ALGO_FALCON512) ? "FN-DSA-512" : "ML-DSA-44";

        string modeStr = ENABLE_ZERO_COPY_PIPELINE ? "ZeroCopy_IOV_Prefetch" : "Baseline_Copy";
        logBenchmarkRecord(modeStr, algoName, dlMetrics, sliceMs, verifierReply.metrics, writerReply.metrics, totalDurationMs);

        double sigDowntimePct = (totalDurationMs > 0)
            ? ((verifierReply.metrics.signatureVerifyUs / 1000.0) / totalDurationMs) * 100.0
            : 0.0;

        cout << "\n==================== OTA PERFORMANCE & PQC BENCHMARK REPORT ====================" << endl;
        cout << " Pipeline Mode               : " << modeStr << endl;
        cout << " Algorithm Tested            : " << algoName << endl;
        cout << " PQC Signature Size          : " << verifierReply.metrics.signatureSize << " Bytes" << endl;
        cout << " PQC Public Key Size         : " << verifierReply.metrics.publicKeySize << " Bytes" << endl;
        cout << " PQC Header Wire Overhead    : " << verifierReply.metrics.pqcOverheadPct << " %" << endl;
        cout << " Signature Verification Time : " << verifierReply.metrics.signatureVerifyUs << " us ("
             << (verifierReply.metrics.signatureVerifyUs / 1000.0) << " ms)" << endl;
        cout << " Sig Verify Downtime Share   : " << fixed << setprecision(4) << sigDowntimePct << " % of total downtime" << endl;
        cout << " Download Time / Speed       : " << setprecision(2) << dlMetrics.totalTimeSec << " s ("
             << (static_cast<double>(dlMetrics.speedDownloadBytesSec) / (1024.0*1024.0)) << " MB/s)" << endl;
        cout << " Header Extraction Time      : " << sliceMs << (ENABLE_ZERO_COPY_PIPELINE ? " us" : " ms") << endl;
        cout << " Stream Decrypt & Hash       : " << verifierReply.metrics.streamDecryptMs << " ms" << endl;
        cout << " Tar Extraction Time         : " << writerReply.metrics.extractionMs << " ms" << endl;
        cout << " Flash Duration / Speed      : " << writerReply.metrics.flashDurationMs << " ms (" << writerReply.metrics.flashThroughputMBs << " MB/s)" << endl;
        cout << " Total End-to-End Downtime   : " << totalDurationMs << " ms" << endl;
        cout << "=================================================================================\n" << endl;

         sysmgr_reboot();
    }
    catch (const exception& e)
    {
        cerr << "[SecureDownloader] Exception occurred: " << e.what() << endl;
        unlink(rawDownloadPath.c_str());
        unlink(zipFilePath.c_str());
        return 1;
    }

    return 0;
}
