import unittest
from tray_win32 import yellow_preview


class PreviewTests(unittest.TestCase):
    def test_valid_and_expired_request(self):
        request = {"action": "test-yellow", "expires_at": 108}
        self.assertTrue(yellow_preview(request, 100))
        self.assertFalse(yellow_preview(request, 108))
        self.assertFalse(yellow_preview(request, 109))

    def test_malformed_or_unbounded_request_is_ignored(self):
        for request in (None, [], {}, {"action": "test-yellow", "expires_at": "oops"},
                        {"action": "test-yellow", "expires_at": 9999}):
            self.assertFalse(yellow_preview(request, 100))


if __name__ == "__main__":
    unittest.main()
