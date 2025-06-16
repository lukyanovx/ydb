RECURSE(downloader)

UNION()

IF(NOT ${COMPAT_INIT_REF})
    SET(COMPAT_INIT_REF stable-24-4)
ENDIF()
IF(NOT ${COMPAT_INTER_REF})
    SET(COMPAT_INTER_REF stable-25-1-2)
ENDIF()
IF(NOT ${COMPAT_TARGET_REF})
    SET(COMPAT_TARGET_REF current)
ENDIF()

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader $COMPAT_INTER_REF/release/ydbd ydbd-last-stable $COMPAT_INTER_REF
    OUT_NOAUTO ydbd-last-stable ydbd-last-stable-name
)

RUN_PROGRAM(
    ydb/tests/library/compatibility/binaries/downloader $COMPAT_INIT_REF/release/ydbd ydbd-prelast-stable $COMPAT_INIT_REF
    OUT_NOAUTO ydbd-prelast-stable ydbd-prelast-stable-name
)

IF(${COMPAT_TARGET_REF} != "current")
    RUN_PROGRAM(
        ydb/tests/library/compatibility/binaries/downloader $COMPAT_TARGET_REF/release/ydbd ydbd-target-stable $COMPAT_TARGET_REF
        OUT_NOAUTO ydbd-target-stable ydbd-target-stable-name
    )
ENDIF()

END()
