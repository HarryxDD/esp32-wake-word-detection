
idf_component_register(
    SRCS ${COMPONENT_DIR}/kissfft/kiss_fft.c
         ${COMPONENT_DIR}/kissfft/kiss_fftr.c
    INCLUDE_DIRS ${COMPONENT_DIR}
    PRIV_INCLUDE_DIRS ${COMPONENT_DIR}/kissfft
)