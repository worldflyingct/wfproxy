package main

import (
    "log"
    "net"
    "io"
    "runtime"
    "strconv"
    "bytes"
    "strings"
    "net/url"
    "crypto/tls"
    "io/ioutil"
    "encoding/json"
)

const defconf = "{\n" +
                "  \"ssl\": false,\n" +
                "  \"crtpath\": \"server.crt\",\n" +
                "  \"keypath\": \"server.key\",\n" +
                "  \"bindport\": 1080,\n" +
                "  \"httphead\": false,\n" +
                "  \"keys\": [{\n" +
                "    \"name\": \"testkey\"\n" +
                "    \"value\": \"D5lfC6LQ1W0BwzP9x3TsxvvdYBCFznqk\"\n" +
                "  }]\n" +
                "}"
type Key struct {
    Name string
    Value string
}
type Config struct {
    Ssl bool
    CrtPath string
    KeyPath string
    BindPort int
    HttpHead bool
    Keys []Key
}
var c Config

func main() {
    runtime.GOMAXPROCS(1)

    log.SetFlags(log.LstdFlags | log.Lshortfile)
    log.Println("version: " + runtime.Version())

    f, err := ioutil.ReadFile("config.json")
    if err != nil {
        err = ioutil.WriteFile("config.json", []byte(defconf), 0777)
        if err != nil {
            log.Println(err)
            return
        }
        f = []byte(defconf)
    }
    err = json.Unmarshal(f, &c)
    if err != nil {
        log.Println(err)
        return
    }
    var ln net.Listener
    if c.Ssl {
        cert, err := tls.LoadX509KeyPair(c.CrtPath, c.KeyPath)
        if err != nil {
            log.Println(err)
            return
        }

        config := &tls.Config{Certificates: []tls.Certificate{cert}}
        ln, err = tls.Listen("tcp", ":" + strconv.Itoa(c.BindPort), config)
        if err != nil {
            log.Println(err)
            return
        }
    } else {
        var err error
        ln, err = net.Listen("tcp", ":" + strconv.Itoa(c.BindPort))
        if err != nil {
            log.Println(err)
            return
        }
    }
    for {
        client, err := ln.Accept()
        if err != nil {
            log.Println(err)
            ln.Close()
            return
        }
        go ProxyRequest(client)
    }
}

func ProxyRequest (client net.Conn) {
    b := make([]byte, 32*1024)
    var n int
    var err error
    if c.HttpHead {
        n, err = client.Read(b)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        headlen := bytes.Index(b[:n], []byte("\r\n\r\n")) + 4
        if headlen < 4 {
            log.Println("no find end flag.")
            client.Close()
            return
        }
        keystart := bytes.Index(b[:headlen], []byte("Authorization: ")) + 15
        if keystart < 15 {
            log.Println("no find auth key.")
            client.Close()
            return
        }
        key := string(b[keystart:keystart + 32])
        check := false
        for _, v := range c.Keys {
            if key == v.Value {
                check = true
                break
            }
        }
        if check == false {
            log.Println("key check fail!!!")
            client.Close()
            return
        }
        _, err = client.Write([]byte("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"))
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
    }
    n, err = client.Read(b)
    if err != nil {
        log.Println(err)
        client.Close()
        return
    }
    if b[0] == 0x05 { //Socks5代理
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
        server, err := net.Dial("tcp", address)
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
    } else { // http代理
        maddr := bytes.IndexByte(b[:n], ' ')
        method := string(b[:maddr])
        host := string(b[maddr+1:maddr+1+bytes.IndexByte(b[maddr+1:n], ' ')])
        if method == "CONNECT" {
            log.Println(host)
            server, err := net.Dial("tcp", host)
            if err != nil {
                log.Println(err)
                client.Close()
                return
            }
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
            host := hostPortURL.Host
            if strings.Index(host, ":") == -1 {
                host += ":80"
            }
            log.Println(host)
            server, err := net.Dial("tcp", host)
            if err != nil {
                log.Println(err)
                client.Close()
                return
            }
            start := bytes.Index(b[:n], []byte(hostPortURL.Host)) + len(hostPortURL.Host)
            if start != -1 {
                copy(b[maddr+1:], b[start:])
                n -= start - maddr - 1
            }
            start = bytes.Index(b[:n], []byte("Proxy-Connection:"))
            if start != -1 {
                copy(b[start:], b[start+6:])
                n -= 6
            }
            _, err = server.Write(b[:n])
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
}

func IoCopy(dst net.Conn, src net.Conn) {
    io.Copy(dst, src)
    dst.Close()
}
