/*
 * Roboception GmbH
 * Munich, Germany
 * www.roboception.com
 *
 * Copyright (c) 2017 Roboception GmbH
 * All rights reserved
 *
 * Author: Heiko Hirschmueller
 */

#include "discover.h"

#include "socket_exception.h"

#include <exception>
#include <ios>
#include <iostream>

#ifdef WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <vector>
#include <future>
#include <string.h>
#include <errno.h>

namespace rcdiscover
{

#ifdef WIN32
typedef SocketWindows SocketImpl;
#else
typedef SocketLinux SocketImpl;
#endif

Discover::Discover() :
  sockets_(SocketType::createAndBindForAllInterfaces(3956))
{
  for (auto &socket : sockets_)
  {
    socket.enableBroadcast();
    socket.enableNonBlocking();
  }
}

Discover::~Discover()
{ }

void Discover::broadcastRequest()
{
  const std::vector<uint8_t> discovery_cmd{0x42, 0x11, 0, 0x02, 0, 0, 0, 1};

  for (auto &socket : sockets_)
  {
    try
    {
      socket.send(discovery_cmd);
    }
    catch(const NetworkUnreachableException &)
    {
      continue;
    }
  }
}

bool Discover::getResponse(std::vector<DeviceInfo> &info,
                           int timeout_per_socket)
{
  // setup waiting for data to arrive

  struct timeval tv;
  tv.tv_sec=timeout_per_socket/1000;
  tv.tv_usec=(timeout_per_socket%1000)*1000;

  // try to get a valid package (repeat if an invalid package is received)

  std::vector<std::future<DeviceInfo>> futures;
  for (auto &socket : sockets_)
  {
    futures.push_back(std::async(std::launch::async, [&socket, &tv]
    {
      DeviceInfo device_info;
      device_info.clear();

      int count = 10;

      auto sock = socket.getHandle<typename SocketType::SocketType>();

      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(sock, &fds);

      while (!device_info.isValid() && count > 0)
      {
        count--;

        if (select(sock+1, &fds, NULL, NULL, &tv) > 0)
        {
          // get package

          uint8_t p[600];

          struct sockaddr_in addr;
#ifdef WIN32
          int naddr = sizeof(addr);
#else
          socklen_t naddr = sizeof(addr);
#endif
          memset(&addr, 0, naddr);

          long n = recvfrom(sock,
                            reinterpret_cast<char *>(p), sizeof(p), 0,
                            reinterpret_cast<struct sockaddr *>(&addr), &naddr);

          // check if received package is a valid discovery acknowledge

          if (n >= 8)
          {
            if (p[0] == 0 && p[1] == 0 && p[2] == 0 &&
                p[3] == 0x03 && p[6] == 0 && p[7] == 1)
            {
              size_t len=(static_cast<size_t>(p[4])<<8)|p[5];

              if (static_cast<size_t>(n) >= len+8)
              {
                // extract information and store in list

                device_info.set(p+8, len);
              }
            }
          }
        }
        else
        {
          count=0;
        }
      }

      return device_info;
    }));

  }

  bool ret = false;
  for (auto &f : futures)
  {
    info.push_back(f.get());
    ret |= info.back().isValid();
  }

  return ret;
}

}
