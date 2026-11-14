import json
import unittest
from codex_light import Lifecycle
from test_monitor import THREAD, event, meta


def question(call_id="q1", count=1, turn="a"):
    return event("item_completed", turn, item={"type": "AgentMessage", "id": call_id,
                 "delivery": "async", "questions": [{"title": "Confirm?"}] * count})


def reply(call_id="q1", index=0, turn="a"):
    body = json.dumps([{"questionItemId": json.dumps(["request_user_input_async", call_id, index]),
                        "answer": "yes"}])
    return event("item_completed", turn, item={"type": "UserMessage", "content": [
        {"type": "text", "text": "<send_user_message_question_reply>\n" + body +
         "\n</send_user_message_question_reply>"}]})


class WaitingTests(unittest.TestCase):
    def setUp(self):
        self.m = Lifecycle(THREAD)
        self.m.consume(meta())
        self.m.consume(event("task_started"))

    def test_question_yellow_answer_red_completion_green(self):
        self.m.consume(question())
        self.assertEqual(self.m.state.state, "waiting")
        self.assertIsNone(self.m.state.value)
        self.m.consume(reply())
        self.assertEqual(self.m.state.value, 1)
        self.m.consume(event("task_complete"))
        self.assertEqual(self.m.state.value, 0)

    def test_all_questions_must_be_answered(self):
        self.m.consume(question(count=2))
        self.m.consume(reply())
        self.assertEqual(self.m.state.pending_count, 1)
        self.assertEqual(self.m.state.state, "waiting")
        self.m.consume(reply(index=1))
        self.assertEqual(self.m.state.value, 1)

    def test_completion_does_not_hide_unanswered_question(self):
        self.m.consume(question())
        self.m.consume(event("task_complete"))
        self.assertEqual(self.m.state.state, "waiting")
        self.m.consume(reply())
        self.assertEqual(self.m.state.value, 0)

    def test_wrong_question_and_wrong_turn_do_not_clear_wait(self):
        self.m.consume(question())
        self.m.consume(reply("unrelated"))
        self.m.consume(reply(turn="old"))
        self.assertEqual(self.m.state.state, "waiting")

    def test_duplicate_question_after_reply_does_not_reopen(self):
        self.m.consume(question())
        self.m.consume(reply())
        self.m.consume(question())
        self.assertEqual(self.m.state.value, 1)

    def test_new_turn_clears_old_question(self):
        self.m.consume(question())
        self.m.consume(event("task_started", "b"))
        self.m.consume(reply())
        self.assertEqual(self.m.state.value, 1)
        self.assertEqual(self.m.state.turn_id, "b")
        self.assertEqual(self.m.state.pending_count, 0)

    def test_interruption_clears_wait_without_green(self):
        self.m.consume(question())
        self.m.consume(event("turn_aborted"))
        self.m.consume(reply())
        self.assertEqual(self.m.state.state, "interrupted")
        self.assertIsNone(self.m.state.value)

    def test_plain_question_and_plain_answer_do_not_guess(self):
        self.m.consume(event("item_completed", item={"type": "AgentMessage", "content": [
            {"type": "Text", "text": "Should I continue?"}]}))
        self.assertEqual(self.m.state.value, 1)
        self.m.consume(question())
        self.m.consume(event("item_completed", item={"type": "UserMessage", "content": [
            {"type": "text", "text": "Here is unrelated text"}]}))
        self.assertEqual(self.m.state.state, "waiting")

    def test_other_thread_question_ignored(self):
        other = question()
        other["payload"]["thread_id"] = "other"
        self.m.consume(other)
        self.assertEqual(self.m.state.value, 1)


if __name__ == "__main__":
    unittest.main()
