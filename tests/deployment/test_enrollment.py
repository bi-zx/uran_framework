import pytest

from uran_deploy.bootstrap import enroll


def test_https_enrollment_device_binding_consumption_and_redirect_denial(https):
    with pytest.raises(ValueError):
        enroll(https["url"] + "/enroll", "one-time", "other_device", https["ca"])
    with pytest.raises(ValueError, match="redirects"):
        enroll(https["url"] + "/redirect-enroll", "one-time", "test_device", https["ca"])
    config = enroll(https["url"] + "/enroll", "one-time", "test_device", https["ca"])
    assert config["auth"]["token"] == "independent-device-token"
    with pytest.raises(ValueError):
        enroll(https["url"] + "/enroll", "one-time", "test_device", https["ca"])
    with pytest.raises(ValueError):
        enroll("http://localhost/enroll", "one-time", "test_device")
