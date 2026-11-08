"""Tests for test/hil/mains_soak.py: cut plan, SNMP parsing and the per-cut checks. No hardware."""

import datetime
import os
import random
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "test", "hil"))
import mains_soak  # noqa: E402
from mains_soak import check_cut, check_idle, parse_mains, plan_cut, plan_gap  # noqa: E402

SNMPGET = (".1.3.6.1.4.1.63050.1.0 = INTEGER: 1\n"
           ".1.3.6.1.4.1.63050.2.0 = Counter32: 14\n"
           ".1.3.6.1.4.1.63050.6.0 = INTEGER: 0\n"
           ".1.3.6.1.2.1.1.3.0 = Timeticks: (123456) 0:20:34.56\n")


def values(status=1, changes=10, alarm=0, uptime=100000):
    return {"status": status, "changes": changes, "alarm": alarm, "uptime": uptime}


def good_cut(seconds=30, **overrides):
    """Arguments of a cut that went right; override one to break it."""
    args = {
        "seconds": seconds,
        "before": values(),
        "loss_after": 0.6,
        "alarm_samples": [(t, 1 if t >= 60 else 0) for t in range(1, int(seconds))],
        "restore_after": 0.7,
        "after": values(changes=12, uptime=100000 + int(seconds * 100) + 500),
    }
    args.update(overrides)
    return args


class PlanTest(unittest.TestCase):
    def test_plan_cut_stays_within_each_kinds_range(self):
        ranges = {kind: (low, high) for kind, _, low, high in mains_soak.CUT_KINDS}
        rng = random.Random(1)
        for _ in range(2000):
            kind, seconds = plan_cut(rng)
            low, high = ranges[kind]
            self.assertTrue(low <= seconds <= high, (kind, seconds))

    def test_plan_cut_uses_every_kind(self):
        rng = random.Random(2)
        seen = {plan_cut(rng)[0] for _ in range(2000)}
        self.assertEqual({kind for kind, _, _, _ in mains_soak.CUT_KINDS}, seen)

    def test_plan_cut_includes_cuts_over_the_alarm_time(self):
        rng = random.Random(3)
        self.assertTrue(any(plan_cut(rng)[1] >= mains_soak.ALARM_EXPECTED_S for _ in range(200)))

    def test_plan_gap_stays_between_min_and_max_minutes(self):
        rng = random.Random(4)
        for _ in range(500):
            self.assertTrue(5 * 60 <= plan_gap(rng, 5, 45) <= 45 * 60)


class ParseTest(unittest.TestCase):
    def test_parse_mains_reads_all_four_values(self):
        self.assertEqual(values(status=1, changes=14, alarm=0, uptime=123456), parse_mains(SNMPGET))

    def test_parse_mains_rejects_missing_value(self):
        self.assertIsNone(parse_mains(SNMPGET.replace(".1.3.6.1.4.1.63050.6.0 = INTEGER: 0\n", "")))

    def test_parse_mains_rejects_timeout_output(self):
        self.assertIsNone(parse_mains("Timeout: No Response from 10.0.0.1.\n"))

    def test_parse_mains_rejects_empty_output(self):
        self.assertIsNone(parse_mains(""))


class CheckCutTest(unittest.TestCase):
    def test_check_cut_accepts_good_medium_cut(self):
        self.assertEqual([], check_cut(**good_cut(30)))

    def test_check_cut_accepts_good_long_cut_with_alarm(self):
        self.assertEqual([], check_cut(**good_cut(120)))

    def test_check_cut_accepts_blip_without_seen_loss(self):
        # A ~1 s outage can fall between polls; the change counter still shows it
        self.assertEqual([], check_cut(**good_cut(1, loss_after=None, alarm_samples=[])))

    def test_check_cut_flags_unseen_loss_on_longer_cut(self):
        problems = check_cut(**good_cut(mains_soak.LOSS_REQUIRED_S, loss_after=None))
        self.assertTrue(any("loss never seen" in p for p in problems), problems)

    def test_check_cut_flags_slow_loss_detection(self):
        problems = check_cut(**good_cut(30, loss_after=mains_soak.DETECT_LIMIT_S + 1))
        self.assertTrue(any("loss seen after" in p for p in problems), problems)

    def test_check_cut_flags_restart(self):
        problems = check_cut(**good_cut(30, after=values(changes=0, uptime=500)))
        self.assertTrue(any("restarted" in p for p in problems), problems)

    def test_check_cut_flags_wrong_change_count(self):
        problems = check_cut(**good_cut(30, after=values(changes=14, uptime=200000)))
        self.assertTrue(any("grew by 4" in p for p in problems), problems)

    def test_check_cut_flags_alarm_before_60_s(self):
        problems = check_cut(**good_cut(50, alarm_samples=[(10, 0), (30, 1), (49, 1)]))
        self.assertTrue(any("before 60 s" in p for p in problems), problems)

    def test_check_cut_allows_alarm_just_at_60_s(self):
        # Our clock starts when the hub confirms; the device's a little later
        self.assertEqual([], check_cut(**good_cut(90, alarm_samples=[(58.5, 0), (59.5, 1), (89, 1)])))

    def test_check_cut_flags_missing_alarm_on_long_cut(self):
        problems = check_cut(**good_cut(120, alarm_samples=[(t, 0) for t in range(1, 120)]))
        self.assertTrue(any("alarm not raised" in p for p in problems), problems)

    def test_check_cut_flags_alarm_raised_too_late(self):
        samples = [(t, 1 if t >= 100 else 0) for t in range(1, 120)]
        problems = check_cut(**good_cut(120, alarm_samples=samples))
        self.assertTrue(any("alarm not raised" in p for p in problems), problems)

    def test_check_cut_flags_unseen_restore(self):
        problems = check_cut(**good_cut(30, restore_after=None))
        self.assertTrue(any("restore never seen" in p for p in problems), problems)

    def test_check_cut_flags_alarm_left_after_restore(self):
        problems = check_cut(**good_cut(120, after=values(changes=12, alarm=1, uptime=200000)))
        self.assertTrue(any("not clean after restore" in p for p in problems), problems)

    def test_check_cut_flags_mains_missing_before(self):
        problems = check_cut(**good_cut(30, before=values(status=0)))
        self.assertTrue(any("not idle before" in p for p in problems), problems)


class CheckIdleTest(unittest.TestCase):
    def test_check_idle_accepts_steady_device(self):
        self.assertEqual([], check_idle(values(), values(uptime=106000), cut_made=False))

    def test_check_idle_flags_restart(self):
        problems = check_idle(values(), values(uptime=300), cut_made=False)
        self.assertTrue(any("restarted" in p for p in problems), problems)

    def test_check_idle_flags_change_nobody_made(self):
        problems = check_idle(values(), values(changes=12, uptime=106000), cut_made=False)
        self.assertTrue(any("without a cut" in p for p in problems), problems)

    def test_check_idle_flags_lost_mains_with_outlet_on(self):
        problems = check_idle(values(), values(status=0, changes=11, uptime=106000), cut_made=True)
        self.assertTrue(any("not normal" in p for p in problems), problems)


class StatsTest(unittest.TestCase):
    def test_summary_says_pass_without_problems(self):
        start = datetime.datetime(2026, 11, 8, 12, 0)
        stats = mains_soak.Stats(start)
        stats.cuts["long"] = 3
        text = stats.text(start + datetime.timedelta(days=7), finished=True)
        self.assertIn("RESULT: PASS", text)
        self.assertIn("Cuts: 3", text)
        self.assertIn("7.0 days", text)

    def test_summary_lists_problems_and_counts_restarts(self):
        start = datetime.datetime(2026, 11, 8, 12, 0)
        stats = mains_soak.Stats(start)
        stats.problem(start, "device restarted (sysUpTime 9 -> 1)")
        text = stats.text(start)
        self.assertIn("1 problems", text)
        self.assertIn("Device restarts: 1", text)
        self.assertIn("device restarted", text)


def row(time="2026-11-08 12:00:00", event="cut", kind="medium", seconds="30", loss="0.4", restore="0.3",
        changes="12", uptime="3600", result="ok"):
    return {"time": time, "event": event, "kind": kind, "seconds_off": seconds, "loss_after_s": loss,
            "restore_after_s": restore, "changes": changes, "uptime_s": uptime, "result": result}


class ReportTest(unittest.TestCase):
    def test_report_says_pass_when_every_cut_is_ok(self):
        html = mains_soak.build_report([row(), row(kind="long", seconds="120")], finished=True)
        self.assertIn("PASS: 2 cuts, no problems", html)
        self.assertNotIn("still running", html)

    def test_report_lists_failed_cut(self):
        html = mains_soak.build_report([row(), row(result="loss never seen in mainsPowerStatus")])
        self.assertIn("1 problems in 2 cuts", html)
        self.assertIn("loss never seen in mainsPowerStatus", html)
        self.assertIn("still running", html)

    def test_report_counts_problem_between_cuts(self):
        html = mains_soak.build_report([row(), row(event="problem", kind="", seconds="", loss="", restore="",
                                                   result="device restarted (sysUpTime 9 -> 1)")])
        self.assertIn("1 problems in 1 cuts", html)
        self.assertIn("device restarted", html)

    def test_report_groups_cuts_by_day(self):
        html = mains_soak.build_report([row(), row(time="2026-11-09 08:00:00"), row(time="2026-11-09 09:00:00")])
        self.assertIn("<td>2026-11-08</td><td>1</td>", html)
        self.assertIn("<td>2026-11-09</td><td>2</td>", html)

    def test_report_shows_uptime_of_last_row_in_days(self):
        html = mains_soak.build_report([row(uptime="86400"), row(uptime="604800")])
        self.assertIn("7.0 days", html)

    def test_report_escapes_text(self):
        html = mains_soak.build_report([row(result="<script>x</script>")])
        self.assertNotIn("<script>x", html)

    def test_report_with_no_rows_still_builds(self):
        self.assertIn("PASS: 0 cuts", mains_soak.build_report([]))


if __name__ == "__main__":
    unittest.main()
