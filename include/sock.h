/*
 * sock.h —— 跨平台套接字头文件（契约文件，冻结后只读）
 *
 * 只做平台差异屏蔽，全部内容都是宏与 include，不产生额外的 .c 文件。
 */
#ifndef CHAT_SOCK_H
#define CHAT_SOCK_H

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef int chat_socklen_t;
    #define CHAT_CLOSE_SOCKET closesocket
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    typedef socklen_t chat_socklen_t;
    #define CHAT_CLOSE_SOCKET close
#endif

#endif /* CHAT_SOCK_H */
