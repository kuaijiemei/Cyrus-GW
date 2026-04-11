from datetime import datetime, timezone


def run() -> str:
    return datetime.now(timezone.utc).isoformat()
