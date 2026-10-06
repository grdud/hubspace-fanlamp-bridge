"""Local setup helpers. No credential cache, browser scraping, or Wi-Fi extraction."""
import asyncio
import getpass
import logging
import time

import aiohttp
from aioafero import v1
import serial


async def hubspace_session(discover=False):
    logging.disable(logging.CRITICAL)
    email = input("Hubspace email: ").strip()
    password = getpass.getpass("Hubspace password (hidden): ")
    async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=60)) as session:
        auth = v1.AferoAuth.for_login(session, email, password)
        del password
        try:
            tokens = await auth.login()
        except v1.OTPRequired:
            tokens = await auth.submit_otp(getpass.getpass("Email verification code: ").strip())
        cloud = v1.AferoBridgeV1(email, tokens.refresh_token, session=session)
        cloud.set_token_data(tokens)
        try:
            account = await cloud.get_account_id()
            devices = await cloud.fetch_discovery_data() if discover else []
            return account, cloud.refresh_token, devices
        finally:
            await cloud.close()


def login(discover=False):
    return asyncio.run(hubspace_session(discover))


def open_serial(port, reset=False):
    conn = serial.Serial(port=None, baudrate=115200, timeout=.2, write_timeout=5)
    conn.dtr = False
    conn.rts = False
    conn.port = port
    conn.open()
    if reset:
        conn.rts = True
        time.sleep(.2)
        conn.rts = False
        time.sleep(1)
    return conn


def fail(error):
    # Authentication exceptions may contain URLs or tokens: print type only.
    print(f"Setup failed ({type(error).__name__}). No credentials were saved to a file.")
