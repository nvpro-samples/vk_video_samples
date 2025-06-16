// The MIT License (MIT)

// Copyright (c) Microsoft Corporation

// Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

// Portions of this repo are provided under the SIL Open Font License.
// See the LICENSE file in individual samples for additional details.

#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <vector>
#include <string>
#include <chrono>

#ifndef _WIN32
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <process.h>
#endif

constexpr char socket_path[14]{"tmpsoc"};

#ifndef _WIN32
constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
using SOCKET = int;
static inline int WSAGetLastError() {
    return errno;
}
#else
static inline const int poll(LPWSAPOLLFD fdArray, ULONG fds, INT timeout) {
    return WSAPoll(fdArray, fds, timeout);
}
static inline const int close(SOCKET ConnectSocket) {
    int result = closesocket(ConnectSocket);
    WSACleanup();
    return result;
}
#endif

static int readDataFromFile(std::string inputCmdsList, std::vector<std::string>& filenames) {
    std::ifstream inputf;
    inputf.open(inputCmdsList);
    if (inputf.is_open()) {
        std::string line;
        while (getline(inputf, line)) {
            filenames.push_back(line);
        }
        filenames.push_back("finish");
        inputf.close();
        return 0;
    }
    std::cout << "Error opening file";
    return -1;
}

int usoc_manager(int isNoPresent, std::string& inputCmdsList) {
    sockaddr addr;
    std::vector<std::string> filenames;
    if (readDataFromFile(inputCmdsList, filenames) == -1) {
        return 1;
    }

    SOCKET listen_sd;
    if ((listen_sd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        exit(-1);
    }

    unlink(socket_path);

    memset(&addr, 0, sizeof(addr));
    addr.sa_family = AF_UNIX;
    strncpy(addr.sa_data, socket_path, sizeof(addr.sa_data) - 1);

    if (bind(listen_sd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind error");
        exit(-1);
    }

    if (listen(listen_sd, 256) == -1) {
        perror("listen error");
        exit(-1);
    }

    pollfd fdarray;
    constexpr int DEFAULT_WAIT = 30000;
    int ret;
    SOCKET asock = INVALID_SOCKET;
    int stop_server = 0;
    for (int i = 0; !stop_server;) {
        fdarray.fd = listen_sd;
        fdarray.events = POLLIN | POLLOUT;
        
        printf("Manager: poll is waiting for incoming events (timeout %d s)\n", DEFAULT_WAIT / 1000);
        if (SOCKET_ERROR == (ret = poll(&fdarray, 1, DEFAULT_WAIT))) {
            printf("Main: poll operation failed: %d\n", WSAGetLastError());
            return 1;
        }

        if (ret == 0) {
            stop_server = 1;
        }

        if (ret) {
            if (fdarray.revents & POLLIN) {
                printf("Manager: Connection established.\n");

                if (INVALID_SOCKET == (asock = accept(listen_sd, NULL, NULL))) {
                    WSAGetLastError();
                    return 1;
                }
                char buf[512] = {0};
                if (SOCKET_ERROR == (ret = recv(asock, buf, sizeof(buf), 0))) {
                    WSAGetLastError();
                    return 1;
                } else
                    printf("Manager: recvd %d bytes\n", ret);
                if (ret) {
                    i = std::min<int>(i, (int)filenames.size() - 1);
                    if (SOCKET_ERROR == (ret = send(asock, filenames[i].c_str(), (int)filenames[i].length() + 1, 0))) {
                        printf("Manager: send socket failed %d \n", WSAGetLastError());
                        return 1;
                    }
                    printf("Manager: sent %d bytes\n", ret);
                }
                if (ret) {
                    if (SOCKET_ERROR == (ret = recv(asock, buf, sizeof(buf), 0))) {
                        WSAGetLastError();
                        return 1;
                    } else if (ret >= 8) {
                        printf("Manager: recvd confirm %d bytes\n", ret);
                        if (strncmp("received", buf, 8) == 0) {
                            i++;
                        }
                    }
                }
            }
        }
    }

    close(fdarray.fd);
    return 0;
}

constexpr int DEFAULT_BUFLEN = 512;

// The following function is a further modification of the main function from the file at the link below:
// https://learn.microsoft.com/en-us/windows/win32/winsock/complete-client-code
int clientConnectServer(std::string& recvbuf, const char* usocfilename) {
    int iResult;
#if defined(_WIN32)
    WSADATA wsaData;
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif
    sockaddr saddr{.sa_family = AF_UNIX};
    strncpy(saddr.sa_data, socket_path, sizeof(socket_path));
    SOCKET ConnectSocket = socket(AF_UNIX, SOCK_STREAM, IPPROTO_IP);
    if (ConnectSocket == INVALID_SOCKET) {
        printf("socket failed with error: %d\n", WSAGetLastError());
        return -1;
    }
    do {
        iResult = connect(ConnectSocket, &saddr, sizeof(saddr) + (int)strlen(saddr.sa_data));
        if (iResult == -1) {
#if defined(_WIN32)
            closesocket(ConnectSocket);
#else
            close(ConnectSocket);
#endif
            ConnectSocket = INVALID_SOCKET;
        }
    } while (iResult == -1);

    if (ConnectSocket == INVALID_SOCKET) {
        printf("Unable to connect to server! %d\n", WSAGetLastError());
#if defined(_WIN32)
        WSACleanup();
#endif
        return -1;
    }
    int hasNewBitstreamReceived = 0;
    int recvbuflen = DEFAULT_BUFLEN;
    std::string sendbuf{"data request"};
    iResult = send(ConnectSocket, sendbuf.c_str(), (int)sendbuf.length(), 0);
    if (iResult == SOCKET_ERROR) {
        printf("send failed with error: %d\n", WSAGetLastError());
        close(ConnectSocket);
        return -1;
    }
    printf("bytes Sent: %d (pid %d)\n", iResult, getpid());
    iResult = recv(ConnectSocket, (char*)recvbuf.c_str(), recvbuflen, 0);
    if (iResult == SOCKET_ERROR) {
        printf("recv failed with error: %d\n", WSAGetLastError());
        close(ConnectSocket);
        return -1;
    }
    if (iResult > 0) {
        hasNewBitstreamReceived = 1;
    }
    sendbuf = {"received"};
    iResult = send(ConnectSocket, (char*)sendbuf.c_str(), (int)sendbuf.length(), 0);
    if (iResult == SOCKET_ERROR) {
        printf("send failed with error: %d\n", WSAGetLastError());
        close(ConnectSocket);
        return -1;
    }
    close(ConnectSocket);
    return hasNewBitstreamReceived;
}