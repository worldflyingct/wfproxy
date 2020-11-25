package main

import (
    "bytes"
    "log"
    "net"
    "io"
    "net/url"
    "runtime"
    "strings"
    "strconv"
)

func main() {
    runtime.GOMAXPROCS(1)

    log.SetFlags(log.LstdFlags | log.Lshortfile)
    log.Println("version: " + runtime.Version())

    go HttpAccept(8888)
    Socks5Accept(8889)
}

func Socks5Accept (port int) {
    tcpaddr := &net.TCPAddr{
        IP: []byte{0, 0, 0, 0},
        Port: port,
        Zone: "",
    }
    l, err := net.ListenTCP("tcp", tcpaddr)
    if err != nil {
        log.Println(err)
        return
    }
    defer l.Close()
    for {
        client, err := l.AcceptTCP()
        if err != nil {
            log.Println(err)
            return
        }
        go Socks5Request(client)
    }
}

func Socks5Request (client *net.TCPConn) {
    client.SetReadBuffer(32*1024)
    client.SetWriteBuffer(32*1024)
    b := make([]byte, 32*1024)
    _, err := client.Read(b)
    if err != nil {
        log.Println(err)
        client.Close()
        return
    }
    if b[0] == 0x05 { //只处理Socks5协议
        // 客户端回应：Socks服务端不需要验证方式
        _, err = client.Write([]byte{0x05, 0x00})
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        n, err := client.Read(b)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        var host string
        switch b[3] {
            case 0x01: //IP V4
                if n <= 9 {
                    client.Close()
                    return
                }
                host = net.IPv4(b[4], b[5], b[6], b[7]).String()
            case 0x03: //域名
                if n <= 7 {
                    client.Close()
                    return
                }
                host = string(b[5 : n-2]) //b[4]表示域名的长度
            case 0x04: //IP V6
                if n <= 21 {
                    client.Close()
                    return
                }
                host = net.IP{b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19]}.String()
            default:
                client.Close()
                return
        }
        address := net.JoinHostPort(host, strconv.Itoa(int(b[n-2])<<8 | int(b[n-1])))
        log.Println(address);
        tcpaddr, err := net.ResolveTCPAddr("tcp", address)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server, err := net.DialTCP("tcp", nil, tcpaddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        _, err = client.Write([]byte{0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) //响应客户端连接成功
        if err != nil {
            log.Println(err)
            client.Close()
            server.Close()
            return
        }
        go IoCopy(client, server)
        go IoCopy(server, client)
    }
}

func HttpAccept (port int) {
    tcpaddr := &net.TCPAddr{
        IP: []byte{0, 0, 0, 0},
        Port: port,
        Zone: "",
    }
    l, err := net.ListenTCP("tcp", tcpaddr)
    if err != nil {
        log.Println(err)
    }
    defer l.Close()
    for {
        client, err := l.AcceptTCP()
        if err != nil {
            log.Println(err)
        }
        go HttpProxyRequest(client)
    }
}

func HttpProxyRequest (client *net.TCPConn) {
    client.SetReadBuffer(32*1024)
    client.SetWriteBuffer(32*1024)
    b := make([]byte, 32*1024)
    n, err := client.Read(b)
    if err != nil {
        log.Println(err)
        client.Close()
        return
    }
    maddr := bytes.IndexByte(b, ' ')
    method := string(b[:maddr])
    host := string(b[maddr+1:maddr+1+bytes.IndexByte(b[maddr+1:], ' ')])
    if method == "CONNECT" {
        log.Println(host)
        tcpaddr, err := net.ResolveTCPAddr("tcp", host)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server, err := net.DialTCP("tcp", nil, tcpaddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server.SetReadBuffer(32*1024)
        server.SetWriteBuffer(32*1024)
        _, err = client.Write([]byte("HTTP/1.1 200 Connection established\r\n\r\n"))
        if err != nil {
            log.Println(err)
            client.Close()
            server.Close()
            return
        }
        go IoCopy(client, server)
        go IoCopy(server, client)
    } else {
        hostPortURL, err := url.Parse(host)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        address := hostPortURL.Host
        if strings.Index(address, ":") == -1 {
            address = address + ":80"
        }
        log.Println(address)
        tcpaddr, err := net.ResolveTCPAddr("tcp", address)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server, err := net.DialTCP("tcp", nil, tcpaddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server.SetReadBuffer(32*1024)
        server.SetWriteBuffer(32*1024)
        httpheadlen := bytes.Index(b, []byte("\r\n\r\n"))+4
        httphead := b[:httpheadlen]
        start := bytes.Index(httphead, []byte(hostPortURL.Host)) + len(hostPortURL.Host)
        if start != -1 {
            httphead = append(httphead[:maddr+1], httphead[start:]...)
        }
        start = bytes.Index(httphead, []byte("Proxy-Connection:"))
        if start != -1 {
            httphead = append(httphead[:start], httphead[start+6:]...)
        }
        b = append(httphead, b[httpheadlen:n]...)
        _, err = server.Write(b)
        if err != nil {
            log.Println(err)
            client.Close()
            server.Close()
            return
        }
        go IoCopy(client, server)
        go IoCopy(server, client)
    }
}

func IoCopy(dst *net.TCPConn, src *net.TCPConn) {
    io.Copy(dst, src)
    dst.Close()
}
