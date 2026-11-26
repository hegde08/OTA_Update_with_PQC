#include "Writer.hpp"

UpdateWriter::UpdateWriter() : IpcServer(WRITER_SERVER_NAME)
{
}

void UpdateWriter::cleanupShmemFiles()
{
	unlink(WRITE_FILE_MINOS.c_str());
	unlink(WRITE_FILE_IFS.c_str());
	unlink("/dev/shmem/Application_OTA_Encrypted.bin");
	unlink("/dev/shmem/Application.sig");
}

void UpdateWriter::onAbortRequested()
{
	if (isBusy.load() && !abortRequested.exchange(true))
	{
		cerr << "\n[Writer] >>> ABORT TRIGGERED: Stopping write operation immediately! <<<\n" << endl;
		closeUart();
		cleanupShmemFiles();
	}
}

bool UpdateWriter::initUart()
{
    uartFd = open(UART_PORT.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (uartFd < 0)
    {
    	cerr << "[Writer] Failed to open UART " << UART_PORT << ". Error: " << strerror(errno) << endl;
    	return false;
    }

    struct termios tty;
    if (tcgetattr(uartFd, &tty) != 0)
    {
		cerr << "[Writer] tcgetattr failed: " << strerror(errno) << endl;
		close(uartFd);
		return false;
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | IHFLOW | OHFLOW);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    tcsetattr(uartFd, TCSANOW, &tty);
    return true;
}

void UpdateWriter::closeUart()
{
    if (uartFd >= 0) close(uartFd);
    uartFd = -1;
}

bool UpdateWriter::sendUartCommand(const std::vector<uint8_t>& txData, std::vector<uint8_t>& rxData, size_t expectedRxLen)
{
	tcflush(uartFd, TCIFLUSH);
	ssize_t bytesWritten = write(uartFd, txData.data(), txData.size());

	if (bytesWritten != (ssize_t)txData.size()) {
		cerr << "   [UART TX] Write failed!" << endl;
		return false;
	}

	rxData.resize(expectedRxLen);
	size_t totalRead = 0;
	int timeoutMs = 2000;

	while (totalRead < expectedRxLen && timeoutMs > 0) {
		ssize_t bytes = read(uartFd, rxData.data() + totalRead, expectedRxLen - totalRead);

		if (bytes > 0) {
			totalRead += bytes;
		} else {
			usleep(10000);
			timeoutMs -= 10;
		}
	}

	return (totalRead == expectedRxLen);
}

bool UpdateWriter::flashStm32(const string& binFile, const string& sigFile, size_t& bytesFlashedOut) {
    bytesFlashedOut = 0;
    if (!initUart()) {
        cerr << "[Writer] Failed to open UART." << endl;
        return false;
    }

    std::vector<uint8_t> rxBuf;
    if(sendUartCommand({CMD_GET_SESSION_INFO}, rxBuf, 2))
    {
    	if(rxBuf[0] == CMD_GET_SESSION_INFO && rxBuf[1] == 0x02)
    	{
    	    cout << "[Writer] Start BL..." << endl;
    	    if (!sendUartCommand({CMD_START_BL}, rxBuf, 2)) return false;
    	    cout << "[Writer] Waiting for STM32 reboot..." << endl;
    	    sleep(2);
    	}
    }
    else
    {
    	return false;
    }

    if (!sendUartCommand({CMD_GET_VER}, rxBuf, 2)) return false;
    if (!sendUartCommand({CMD_START_FLASH}, rxBuf, 2)) return false;
    if (!sendUartCommand({CMD_FLASH_ERASE, 0x08, 0x08}, rxBuf, 2)) return false;

    cout << "[Writer] Flashing Application Frames..." << endl;
    ifstream inFile(binFile, ios::binary);
    uint8_t currentFrame = 1;

    while (inFile) {
    	if (abortRequested.load())
    	{
			cerr << "[Writer] Flashing aborted: Client died!" << endl;
			closeUart();
			return false;
    	}

        std::vector<uint8_t> chunk(256, 0xFF);
        inFile.read(reinterpret_cast<char*>(chunk.data()), 256);
        size_t bytesRead = inFile.gcount();
        if (bytesRead == 0) break;

        std::vector<uint8_t> txData = {CMD_MEM_WRITE, currentFrame};
        txData.insert(txData.end(), chunk.begin(), chunk.begin() + bytesRead);

        if (!sendUartCommand(txData, rxBuf, 3)) return false;
        bytesFlashedOut += bytesRead;
        currentFrame++;
    }

    ifstream sigStream(sigFile, ios::binary);
    if (!sigStream) {
        closeUart();
        return false;
    }

    std::vector<uint8_t> sigData((std::istreambuf_iterator<char>(sigStream)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> txDataVerify = {CMD_MEM_VERIFY};
    txDataVerify.insert(txDataVerify.end(), sigData.begin(), sigData.end());

    if (!sendUartCommand(txDataVerify, rxBuf, 2)) {
        closeUart();
        return false;
    }

    bool verified = (rxBuf[1] == 0x00);
    closeUart();
    return verified;
}

bool UpdateWriter::extractPackage(const string& zipFilepath, uint64_t& outExtractMs)
{
    cout << "[Writer] Extracting " << zipFilepath << " to RAM..." << endl;
    auto tStart = chrono::steady_clock::now();
    string cmd = "tar -xf " + zipFilepath + " -C /dev/shmem/";
    int ret = system(cmd.c_str());
    auto tEnd = chrono::steady_clock::now();
    outExtractMs = chrono::duration_cast<chrono::milliseconds>(tEnd - tStart).count();
    return (ret == 0);
}

bool UpdateWriter::flashPartition(const string& sourceFile, const string& targetDevice, size_t& bytesWrittenOut)
{
    bytesWrittenOut = 0;
    cout << "[Writer] Flashing " << sourceFile << " -> " << targetDevice << "..." << endl;

    int srcFd = open(sourceFile.c_str(), O_RDONLY);
    if (srcFd < 0) {
        cerr << "[Writer] Failed to open source file: " << sourceFile << endl;
        return false;
    }

    int destFd = open(targetDevice.c_str(), O_WRONLY | O_SYNC);
    if (destFd < 0) {
        cerr << "[Writer] Failed to open target block device: " << targetDevice << endl;
        close(srcFd);
        return false;
    }

    char buffer[65536];
    ssize_t bytesRead, bytesWritten;
    bool success = true;

    while ((bytesRead = read(srcFd, buffer, sizeof(buffer))) > 0)
    {
    	if (abortRequested.load())
    	{
			cerr << "[Writer] Block write aborted!" << endl;
			success = false;
			break;
    	}

        bytesWritten = write(destFd, buffer, bytesRead);
        if (bytesWritten != bytesRead)
        {
            cerr << "[Writer] Block write failed!" << endl;
            success = false;
            break;
        }
        bytesWrittenOut += bytesWritten;
    }

    if (bytesRead < 0) success = false;
    if (success) fsync(destFd);

    close(srcFd);
    close(destFd);
    return success;
}

bool UpdateWriter::getBankInfo()
{
	cout << "[Writer] Reading " << UENV_FILE << endl;
	bool status = false;

	ifstream envFile(UENV_FILE);
	if (!envFile.is_open()) return false;

	string line;
	string val;

	while(getline(envFile, line))
	{
		if(line.find("ota_bank=")==0)
		{
			val = line.substr(9);
			if(!val.empty())
			{
				envInfo.activeBank = val[0];
				status = true;
			}
		}
		else if(line.find("ifs_sectors_a=")==0)
		{
			val = line.substr(14);
			if(!val.empty())
			{
				envInfo.ifs_Len_A = (uint32_t)strtoul(val.c_str(), nullptr, 16);
			}
		}
		else if(line.find("ifs_sectors_b=")==0)
		{
			val = line.substr(14);
			if(!val.empty())
			{
				envInfo.ifs_Len_B = (uint32_t)strtoul(val.c_str(), nullptr, 16);
			}
		}
	}
	envFile.close();

	cout << "[Writer] Active bank: " << envInfo.activeBank << endl;
	return status;
}

size_t UpdateWriter::getFileSize(const string& path) const
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (size_t)st.st_size;
}

uint32_t UpdateWriter::calcIFSSectors(const string& ifsPath) const
{
    size_t size = getFileSize(ifsPath);
    if (size == 0) return 0;
    return (uint32_t)((size + 511) / 512);
}

bool UpdateWriter::setUBootBankEnv()
{
	cout << "[Writer] Setting Passive Bank: " << envInfo.passiveBank << endl;
    ofstream outFile(UENV_FILE, ios::trunc);
    if (!outFile.is_open()) return false;

    char hexBuf[32];
    outFile << "ota_bank=" << envInfo.passiveBank << "\n";
    snprintf(hexBuf, sizeof(hexBuf), "0x%X", envInfo.ifs_Len_A);
    outFile << "ifs_sectors_a=" << hexBuf << "\n";
    snprintf(hexBuf, sizeof(hexBuf), "0x%X", envInfo.ifs_Len_B);
    outFile << "ifs_sectors_b=" << hexBuf << "\n";

    outFile.flush();
    outFile.close();
    return true;
}

bool UpdateWriter::mountFatPartition()
{
    if (mount(FAT_PARTITION.c_str(), MOUNT_BOOT.c_str(), 0, "dos", nullptr, 0) != 0)
    {
        cerr << "[Writer] Mount FAT32 failed: " << strerror(errno) << endl;
        return false;
    }
    return true;
}

void UpdateWriter::unmountFatPartition()
{
	sync();
	umount(MOUNT_BOOT.c_str(), 0);
}

bool UpdateWriter::flashPartitionWrapper(size_t& totalBytesFlashed)
{
	totalBytesFlashed = 0;
	size_t written = 0;

	if (envInfo.activeBank == 'B')
	{
		if (!flashPartition(WRITE_FILE_MINOS, PARTITION_A_MINOS, written)) return false;
		totalBytesFlashed += written;
		unlink(WRITE_FILE_MINOS.c_str());

		if (!flashPartition(WRITE_FILE_IFS, PARTITION_A_IFS, written)) return false;
		totalBytesFlashed += written;
		unlink(WRITE_FILE_IFS.c_str());
	}
	else if (envInfo.activeBank == 'A')
	{
		if (!flashPartition(WRITE_FILE_MINOS, PARTITION_B_MINOS, written)) return false;
		totalBytesFlashed += written;
		unlink(WRITE_FILE_MINOS.c_str());

		if (!flashPartition(WRITE_FILE_IFS, PARTITION_B_IFS, written)) return false;
		totalBytesFlashed += written;
		unlink(WRITE_FILE_IFS.c_str());
	}
	else
	{
		return false;
	}
	return true;
}

void UpdateWriter::processMessage(int rcvid, const UpdateMsg& msg)
{
    isBusy.store(true);
	abortRequested.store(false);

    if (msg.type == MSG_WRITE_PARTITION)
    {
        cout << "[Writer] OTA Write Sequence: " << msg.filepath << endl;

        ExecutionMetrics metrics{};
        bool success = true;
		string zipPath = msg.filepath;

		if (!mountFatPartition() || abortRequested.load()) success = false;
		if (success && (!getBankInfo() || abortRequested.load())) success = false;

		uint64_t extractTime = 0;
		if (success && (!extractPackage(zipPath, extractTime) || abortRequested.load())) {
			success = false;
		}
		metrics.extractionMs = extractTime;
		unlink(zipPath.c_str());

		if (success && !abortRequested.load())
		{
			if (envInfo.activeBank == 'A') {
				envInfo.ifs_Len_B = calcIFSSectors(WRITE_FILE_MINOS);
				envInfo.passiveBank = 'B';
			} else {
				envInfo.ifs_Len_A = calcIFSSectors(WRITE_FILE_MINOS);
				envInfo.passiveBank = 'A';
			}
		}

		size_t bytesFlashed = 0;
		auto flashStart = chrono::steady_clock::now();
		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_FLASH_GATEWAY_START, "FLASH_GATEWAY_START");

		if (success && (!flashPartitionWrapper(bytesFlashed) || abortRequested.load()))
		{
			success = false;
		}

		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_FLASH_GATEWAY_END, "FLASH_GATEWAY_END");
		auto flashEnd = chrono::steady_clock::now();
		metrics.flashDurationMs = chrono::duration_cast<chrono::milliseconds>(flashEnd - flashStart).count();

		if (metrics.flashDurationMs > 0 && bytesFlashed > 0) {
			metrics.flashThroughputMBs = (bytesFlashed / (1024.0 * 1024.0)) / (metrics.flashDurationMs / 1000.0);
		}

		if (success && !abortRequested.load())
		{
			if (!setUBootBankEnv()) success = false;
		}

		cleanupShmemFiles();
	    unmountFatPartition();

        isBusy.store(false);
		sendReply(rcvid, success ? 0 : -1, metrics);
    }
    else if (msg.type == MSG_WRITE_STM32_UART)
    {
    	cout << "[Writer] STM32 OTA via UART" << endl;
    	ExecutionMetrics metrics{};
    	string zipPath = msg.filepath;

		uint64_t extractTime = 0;
		if (!extractPackage(zipPath, extractTime)) {
			unlink(zipPath.c_str());
            isBusy.store(false);
			sendReply(rcvid, -1);
			return;
		}
		metrics.extractionMs = extractTime;
		unlink(zipPath.c_str());

		string binPath = "/dev/shmem/Application_OTA_Encrypted.bin";
		string sigPath = "/dev/shmem/Application.sig";

		size_t bytesFlashed = 0;
		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_FLASH_END_ECU_START, "FLASH_END_ECU_START");

		auto flashStart = chrono::steady_clock::now();
		bool ok = (!abortRequested.load()) && flashStm32(binPath, sigPath, bytesFlashed);

		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_FLASH_END_ECU_END, "FLASH_END_ECU_END");
		auto flashEnd = chrono::steady_clock::now();
		metrics.flashDurationMs = chrono::duration_cast<chrono::milliseconds>(flashEnd - flashStart).count();

		cleanupShmemFiles();
        isBusy.store(false);
		sendReply(rcvid, ok ? 0 : -1, metrics);
    }
    else
    {
        isBusy.store(false);
        MsgError(rcvid, ENOSYS);
    }
}

int main()
{
	try
	{
		UpdateWriter writer;
		writer.run();
	}
	catch (const exception& e)
	{
		cerr << "Writer Fatal Error: " << e.what() << endl;
		return 1;
	}
	return 0;
}
