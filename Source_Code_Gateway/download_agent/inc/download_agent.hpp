/*
 * download_agent.hpp
 *
 *  Created on: Jul 27, 2026
 *      Author: hegde
 */

#ifndef INC_DOWNLOAD_AGENT_HPP_
#define INC_DOWNLOAD_AGENT_HPP_

#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/sysmgr.h>
#include <chrono>
#include "shared_ipc.hpp"

struct DownloadMetrics {
    double     totalTimeSec;
    curl_off_t speedDownloadBytesSec;
    curl_off_t downloadedBytes;
};

class SecureDownloader {
private:
    CURL* curl;
    const std::string versionFilePath;

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, FILE* stream);
	std::vector<int> parseVersionTokens(const std::string& verStr) const;
	int compareVersions(const std::string& incomingVer, const std::string& currentVer) const;
	bool readCurrentVersion(const std::string& verFilePath, std::string& outVersion) const;

public:
	SecureDownloader();
    ~SecureDownloader();

    bool download(const string& url, const string& outPath,
                  const string& cert, const string& key, const string& ca,
                  DownloadMetrics& metrics);

    // Baseline slicing method
	bool stripHeaderAndExtractZip(const string& downloadedFile, 
                                  const string& outZipFile,
                                  OtaHeader& outHeader,
                                  uint64_t& sliceDurationMs);

	// Zero-copy in-place header parser (0 RAM overhead)
	bool parseHeaderInPlace(const string& downloadedFile,
                            OtaHeader& outHeader,
                            uint32_t& outPayloadOffset);

	bool validateVersion(const OtaHeader& header,
	                     const std::string& verFilePath,
	                     std::string& outIncomingVersion);
};

#endif /* INC_DOWNLOAD_AGENT_HPP_ */
