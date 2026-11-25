# fck_aos - the owner: one Active Object, AO_Fck, that finds the module,
# holds it and decides when anything happens on it. fck_ao.c is the Active
# Object, on kode-sdk's QP/C (kode_sys.h), with its worker on core 1;
# fck_aos.c is what its signals do. The only layer above the drivers that
# may wait, and it waits on its worker, never in a step.
#
# Laid out as kaos is: what the facade includes in core/include, and each AO
# in a folder of its own with its sources and private headers in src/. There
# is one AO here, AO_Fck, so there is one folder.
idf_component_register(
    SRCS "fck/src/fck_aos.c"
         "fck/src/fck_ao.c"
         "fck/src/fck_plan.c"
    INCLUDE_DIRS "core/include"
    PRIV_INCLUDE_DIRS "fck/src"
    REQUIRES kdefs fck_drv
    PRIV_REQUIRES kode freertos fck_bsp
)
