RECURSE(downloader)

UNION()

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader 25.1.1.18/release/ydbd ydbd-last-stable 25.1.1.18
    OUT_NOAUTO ydbd-last-stable ydbd-last-stable-name
)

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader 25-1-1-13-1/release/ydbd ydbd-prelast-stable 25-1-1-13-1
    OUT_NOAUTO ydbd-prelast-stable ydbd-prelast-stable-name
)

END()
