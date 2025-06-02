RECURSE(downloader)

UNION()

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader dev-mfilitov-25-1-1-11/release/ydbd ydbd-last-stable 25-1-1-11
    OUT_NOAUTO ydbd-last-stable ydbd-last-stable-name
)

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader dev-mfilitov-24-4-4-hotfix-12/release/ydbd ydbd-prelast-stable 24-4-4-hotfix-12
    OUT_NOAUTO ydbd-prelast-stable ydbd-prelast-stable-name
)

END()
