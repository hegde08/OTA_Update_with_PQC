/*
 * Writer.hpp
 *
 *  Created on: Jul 27, 2026
 *      Author: hegde
 */

#ifndef INC_WRITER_HPP_
#define INC_WRITER_HPP_

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <cstdlib>
#include <termios.h>
#include <vector>
#include <atomic>
#include <chrono>

#include "shared_ipc.hpp"

class UpdateWriter : public IpcServer {

private:
	const string PARTITION_A_MINOS = "/dev/sd0.lnxdata.1";
	const string PARTITION_A_IFS   = "/dev/sd0.lnxdata.3";
	const string PARTITION_B_MINOS = "/dev/sd0.lnxdata.2";
	const string PARTITION_B_IFS   = "/dev/sd0.lnxdata.4";

	const string WRITE_FILE_MINOS  = "/dev/shmem/ifs-rpi4.ui";
	const string WRITE_FILE_IFS    = "/dev/shmem/rootfs.img";

	const string FAT_PARTITION     = "/dev/sd0.ms.0";
	const string MOUNT_BOOT        = "/mnt/boot";
	const string UENV_FILE         = "/mnt/boot/uEnv.txt";

	int uartFd = -1;
	const string UART_PORT         = "/dev/ser2";

	static constexpr uint8_t CMD_GET_VER          = 0x80;
	static constexpr uint8_t CMD_START_FLASH      = 0x81;
	static constexpr uint8_t CMD_FLASH_ERASE      = 0x82;
	static constexpr uint8_t CMD_MEM_WRITE        = 0x83;
	static constexpr uint8_t CMD_MEM_VERIFY       = 0x84;
	static constexpr uint8_t CMD_START_BL         = 0x85;
	static constexpr uint8_t CMD_GET_SESSION_INFO = 0x86;

	std::atomic<bool> isBusy{false};
	std::atomic<bool> abortRequested{false};

	struct uEnvInfo
	{
		char activeBank = 0;
		char passiveBank = 0;
		uint32_t ifs_Len_A = 0;
		uint32_t ifs_Len_B = 0;
	};

	uEnvInfo envInfo;

	bool extractPackage(const string& zipFilepath, uint64_t& outExtractMs);
	bool flashPartition(const string& sourceFile, const string& targetDevice, size_t& bytesWrittenOut);
	bool setUBootBankEnv();
	bool mountFatPartition();
	void unmountFatPartition();
	bool getBankInfo();
	bool flashPartitionWrapper(size_t& totalBytesFlashed);
	uint32_t calcIFSSectors(const string& ifsPath) const;
	size_t getFileSize(const string& path) const;

	bool initUart();
	void closeUart();
	bool sendUartCommand(const std::vector<uint8_t>& txData, std::vector<uint8_t>& rxData, size_t expectedRxLen);
	bool flashStm32(const string& binFile, const string& sigFile, size_t& bytesFlashedOut);

	void cleanupShmemFiles();

public:
    UpdateWriter();
    void processMessage(int rcvid, const UpdateMsg& msg) override;
    void onAbortRequested() override;
};

#endif /* INC_WRITER_HPP_ */
