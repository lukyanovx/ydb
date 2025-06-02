RECURSE(downloader)

UNION()

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader stable-25-1-1/release/ydbd ydbd-last-stable 25-1-1
    OUT_NOAUTO ydbd-last-stable ydbd-last-stable-name
)

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader dev-mfilitov-25-1-runtime-version-59/release/ydbd ydbd-prelast-stable 25-1-runtime-version-59
    OUT_NOAUTO ydbd-prelast-stable ydbd-prelast-stable-name
)

END()
