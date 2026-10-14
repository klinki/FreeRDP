#!/usr/bin/env python3
"""Write a birth-time receipt, then exec an explicitly supplied owned process.

Used inside the isolated comparison container. Standard input is inherited;
credentials are never read, logged, passed in arguments, or written here.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--executable', required=True)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.receipt.exists():
        raise ValueError('owned process receipt exists; refusing to replace it')
    executable = Path(args.executable).resolve(strict=True)
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    pid = os.getpid()
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    receipt = dict(owner=args.owner, pid=pid, start_ticks=int(fields[19]),
                   executable=str(executable), arguments=arguments,
                   started_utc=datetime.now(timezone.utc).isoformat(),
                   linux_unix_s=time.time(), linux_monotonic_ns=time.monotonic_ns())
    temporary = args.receipt.with_suffix(args.receipt.suffix + '.tmp')
    temporary.write_text(json.dumps(receipt, indent=2, allow_nan=False) + '\n')
    temporary.replace(args.receipt)
    os.execv(str(executable), [str(executable), *arguments])


if __name__ == '__main__':
    main()
