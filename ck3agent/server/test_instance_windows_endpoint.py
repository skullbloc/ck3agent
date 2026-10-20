"""instance.windows_endpoint_path mirrors the Windows bridge's state-dir rule
(bridge/src/os/win_paths.cpp). The cases are those of the C++ rule test,
bridge/src/os/test/test_os_win_paths_rules.cpp, read for the state dir (the
bridge writes endpoint.json there); a change to one side must change both.
"""
import pytest

from ck3agent.server.instance import (
    InvalidInstanceId, bridge_endpoint, windows_endpoint_path)


def test_default_instance_is_directly_under_localappdata():
    assert windows_endpoint_path({"LOCALAPPDATA": "C:\\Users\\u\\AppData\\Local"}) == \
        "C:\\Users\\u\\AppData\\Local\\ck3agent\\endpoint.json"


def test_named_instance_is_under_inst():
    assert windows_endpoint_path({"LOCALAPPDATA": "C:\\LA", "CK3_INSTANCE_ID": "arm3"}) == \
        "C:\\LA\\ck3agent\\inst\\arm3\\endpoint.json"


def test_userprofile_when_no_localappdata():
    assert windows_endpoint_path({"USERPROFILE": "C:\\Users\\u"}) == \
        "C:\\Users\\u\\AppData\\Local\\ck3agent\\endpoint.json"


def test_instance_root_overrides_inst():
    assert windows_endpoint_path({"LOCALAPPDATA": "C:\\LA", "CK3_INSTANCE_ROOT": "E:\\runs",
                                  "CK3_INSTANCE_ID": "arm3"}) == "E:\\runs\\arm3\\endpoint.json"


def test_no_base_directory_is_none():
    assert windows_endpoint_path({}) is None


def test_a_non_slug_id_raises_here():
    # The bridge falls back to `default` and reports a config error; a client
    # refuses instead (instance.py's rule everywhere), so it never connects to a
    # default instance it was not asked for.
    with pytest.raises(InvalidInstanceId):
        windows_endpoint_path({"LOCALAPPDATA": "C:\\LA", "CK3_INSTANCE_ID": "bad id"})


def test_explicit_endpoint_wins_and_linux_stays_unix(monkeypatch):
    assert bridge_endpoint({"CK3_BRIDGE_ENDPOINT": "/tmp/x/endpoint.json"}) == "/tmp/x/endpoint.json"
    monkeypatch.setattr("os.name", "posix")
    assert bridge_endpoint({"LOCALAPPDATA": "C:\\LA"}) is None
