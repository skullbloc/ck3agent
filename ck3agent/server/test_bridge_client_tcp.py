"""BridgeClient's TCP-loopback transport (ADR 0001, the Windows bridge).

A fake bridge on 127.0.0.1 reads the first line and answers as the Windows
bridge does (ipc_server.cpp): `{"type":"auth_ok"}` for the token in
endpoint.json, `{"type":"auth_refused"}` and a close otherwise. Without an
endpoint the client keeps its AF_UNIX path (nothing here touches it).
"""
import json
import socket
import threading

from ck3agent.server.bridge_client import BridgeClient


class FakeBridge:
    def __init__(self, token: str):
        self.token = token
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.first_line = None
        self.after = []
        self.t = threading.Thread(target=self._serve, daemon=True)
        self.t.start()

    def _serve(self):
        conn, _ = self.srv.accept()
        f = conn.makefile("rb")
        self.first_line = json.loads(f.readline())
        if self.first_line.get("auth") != self.token:
            conn.sendall(b'{"type":"auth_refused"}\n')
            conn.close()
            return
        conn.sendall(b'{"type":"auth_ok"}\n')
        line = f.readline()
        if line:
            self.after.append(json.loads(line))
            conn.sendall(b'{"type":"pong"}\n')
        conn.close()

    def endpoint_file(self, tmp_path, host="127.0.0.1", transport="tcp_loopback"):
        p = tmp_path / "endpoint.json"
        p.write_text(json.dumps({"transport": transport, "endpoint": f"{host}:{self.port}",
                                 "token": self.token, "pid": 1234}))
        return str(p)


def test_token_line_first_then_the_protocol(tmp_path):
    fb = FakeBridge("ab" * 32)
    c = BridgeClient(socket_path=str(tmp_path / "unused.sock"), endpoint=fb.endpoint_file(tmp_path))
    assert c.connect(timeout=5) is True
    assert fb.first_line == {"auth": "ab" * 32}
    c.send({"type": "ping"})
    assert c.recv(timeout=5) == {"type": "pong"}
    fb.t.join(5)
    assert fb.after == [{"type": "ping"}]
    c.close()


def test_a_refused_token_is_a_failed_connect(tmp_path):
    fb = FakeBridge("ab" * 32)
    path = fb.endpoint_file(tmp_path)
    data = json.loads(open(path).read()); data["token"] = "cd" * 32
    open(path, "w").write(json.dumps(data))
    c = BridgeClient(socket_path=str(tmp_path / "unused.sock"), endpoint=path)
    assert c.connect(timeout=5) is False
    assert c.sock is None


def test_a_non_loopback_endpoint_is_refused_before_connecting(tmp_path):
    fb = FakeBridge("ab" * 32)
    c = BridgeClient(socket_path=str(tmp_path / "unused.sock"),
                     endpoint=fb.endpoint_file(tmp_path, host="10.0.0.5"))
    assert c.connect(timeout=1) is False
    assert fb.first_line is None          # nothing was sent anywhere


def test_another_transport_is_refused(tmp_path):
    fb = FakeBridge("ab" * 32)
    c = BridgeClient(socket_path=str(tmp_path / "unused.sock"),
                     endpoint=fb.endpoint_file(tmp_path, transport="named_pipe"))
    assert c.connect(timeout=1) is False


def test_no_endpoint_keeps_the_unix_socket_path(tmp_path):
    c = BridgeClient(socket_path=str(tmp_path / "missing.sock"))
    assert c.endpoint is None
    assert c.connect(timeout=1) is False   # AF_UNIX path: the socket does not exist
