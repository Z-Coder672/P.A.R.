#!/usr/bin/env python3
"""Send a synthetic moderation email so you can eyeball the layout and
confirm SMTP creds work end-to-end. Run from YT-Streamer/ with the venv
active:  python test_mod_email.py
"""
import os
import sys
from pathlib import Path
from dotenv import load_dotenv

load_dotenv(Path(__file__).parent / ".env")

# Reuse the real implementation
sys.path.insert(0, str(Path(__file__).parent))
from YT_streamer import send_mod_email, NOTIFY_EMAIL, SMTP_HOST, SMTP_USER, SMTP_PASS

# 84-byte all-cyan-on-one-row sample bitmap (37x18, MSB-first) as base64.
# Just enough valid PNG-ish data for the email to render the inline image;
# the real bytes don't matter for SMTP testing.
SAMPLE_B64 = (
    "iVBORw0KGgoAAAANSUhEUgAAACUAAAASCAYAAADxXn7uAAAAOUlEQVR42u3PMQEAAAjDMMC/"
    "5+ECvlRA00nq2QkICAgICAgICAgICAgICAgICAgICAgICAgIfBxYxgABe5sM4QAAAABJRU5ErkJggg=="
)

print(f"  NOTIFY_EMAIL = {NOTIFY_EMAIL!r}")
print(f"  SMTP_HOST    = {SMTP_HOST!r}")
print(f"  SMTP_USER    = {SMTP_USER!r}")
print(f"  SMTP_PASS    = {'<set>' if SMTP_PASS else '<EMPTY>'}")

if not SMTP_PASS:
    print("\nERROR: SMTP_PASS is empty in .env — set it before running this test.")
    sys.exit(1)

fake_image_result = {
    "verdict": "approve",
    "confidence": 0.92,
    "flags": [],
    "reasoning": "Benign abstract pixel shape, no policy concerns.",
}
fake_name_result = {
    "verdict": "review",
    "confidence": 0.6,
    "flags": ["ambiguous"],
    "reasoning": "Name is borderline — could be edgy humor or a slur.",
}

# Verbose path — talk to the server directly so we can see codes.
import smtplib
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

msg = MIMEMultipart("alternative")
msg["Subject"] = "[PAR Mod TEST] diagnostic"
msg["From"]    = SMTP_USER
msg["To"]      = NOTIFY_EMAIL
msg.attach(MIMEText("Plain-text diagnostic body.", "plain"))
msg.attach(MIMEText(
    "<html><body><h2>PAR mod test</h2>"
    f"<p>From {SMTP_USER} via {SMTP_HOST}:465</p></body></html>", "html"))

print("\n--- SMTP transaction ---")
try:
    with smtplib.SMTP_SSL(SMTP_HOST, 465, timeout=30) as smtp:
        smtp.set_debuglevel(1)
        smtp.ehlo()
        smtp.login(SMTP_USER, SMTP_PASS)
        refused = smtp.send_message(msg)
        print("\nRefused recipients:", refused)
    print("\nResult: server accepted the message (250 OK above means delivered to MTA).")
    print("If it doesn't arrive: check Gmail SPAM folder, then check SPF/DKIM/DMARC")
    print("on par.zimmzimm.com — Gmail silently drops unauthenticated mail from")
    print("subdomains lacking these records.")
except Exception as e:
    print(f"\nFAILED: {type(e).__name__}: {e}")
    sys.exit(2)
