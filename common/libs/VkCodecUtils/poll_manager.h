#include <vector>
#include <string>
#include <iostream>
#include <sstream>

enum IPC_TYPE { UNIX_DOMAIN_SOCKETS = 0 };
constexpr int DEFAULT_BUFLEN = 512;

int usoc_manager(int isNoPresent, std::string& inputCmdsList);
int clientConnectServer(std::string& recvbuf, const char* usocfilename = NULL);

#ifdef _WIN32

static int cloneTheProcess(int argc, const char** argv, PROCESS_INFORMATION& pi, STARTUPINFO& si) {
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    std::string argsToPass;
    for (int i = 0; i < argc; i++) {
        argsToPass += argv[i];
        argsToPass += " ";
    }
    argsToPass += "spawn";
    if (!CreateProcess(NULL, (LPTSTR)argsToPass.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed (%d).\n", GetLastError());
        return -1;
    }
    return 0;
}
#endif

static int parseCharArray(std::vector<std::string>& w, const char* messageString, int& argc, const char** argv) {
    std::stringstream ss(messageString);
    std::string word;
    argc = 0;
    std::cout << std::endl;
    while (ss >> w[argc]) {
        if (w[argc][0] == '~') {
            w[argc] = getenv("HOME") + w[argc].substr(1);
        }
        if (w[argc].substr(0, 6) == "finish") {
            printf("Received a request to finish this decode worker. The worker process is terminated (completed).\n");
            return 0;
        }
        if (w[argc].substr(0, 6) == "nodata") {
            printf("Received a request to wait for a data...\n");
            return 0;
        }
        argv[argc] = w[argc].c_str();
        argc++;
    }
    return argc >= 1;
}

static int receiveNewBitstream(IPC_TYPE ipcType, bool enableWorkerProcessesPoll, std::string& receivedMessage) {
    if (!enableWorkerProcessesPoll) {
        return 0;
    }
    int isDataReceived = 0;
    if (ipcType == IPC_TYPE::UNIX_DOMAIN_SOCKETS) {
        isDataReceived = clientConnectServer(receivedMessage);
    }
    return isDataReceived;
}