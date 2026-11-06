"""Run the update site: python -m rfupdate [--host H] [--port P] [--data DIR] [--verbose]

The admin password comes from the environment variable RFUPDATE_ADMIN_PASSWORD
(without it nobody can log in; the device list and check-ins still work).
"""

import argparse
import os

from .store import Store
from .web import make_server


def main():
    ap = argparse.ArgumentParser(description="RF-Power-Monitor update site")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default all)")
    ap.add_argument("--port", type=int, default=8071)
    ap.add_argument("--data", default=os.environ.get("RFUPDATE_DATA", "/data"),
                    help="directory for the database and releases (default $RFUPDATE_DATA or /data)")
    ap.add_argument("--verbose", action="store_true", help="log every request")
    args = ap.parse_args()
    os.makedirs(args.data, exist_ok=True)
    store = Store(args.data)
    password = os.environ.get("RFUPDATE_ADMIN_PASSWORD") or None
    server = make_server(store, args.host, args.port, password, verbose=args.verbose)
    print("rfupdate on %s:%d, data in %s%s" % (args.host, args.port, args.data,
                                               "" if password else " (no admin password: logins off)"), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


if __name__ == "__main__":
    main()
