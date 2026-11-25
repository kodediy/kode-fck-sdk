# fck - what an app includes, in two layers beside kode-sdk's own two:
#
#   fck.h      the standard layer, for tasks (fck_standard.c, fck_events.c): every call
#              waits for the owner and answers a bool, with fck_error() for why
#   fck_sys.h  the extended layer, for Active Objects: the component fck_sys, which
#              carries each request out through the core below and answers the Active
#              Object with a signal
#
# Both stand on the core (core/include/fck_core.h, the rest of src/): the facade as it was before the
# layers - every call in kode-sdk's kode_status_t - which asks fck_aos for everything and names
# no driver type, so a driver can change under it without an app noticing.
#
# fck_bsp is public because fck.h names the module's flash maps (fck_flash()); the owner is
# private, so no app ever sees its types (tools/check_layers.sh, rule 6). Laid out as kode
# is: the header in include/, the sources in src/.
idf_component_register(
    SRCS "src/fck_standard.c"
         "src/fck_events.c"
         "src/fck.c"
         "src/fck_radio.c"
         "src/fck_flash.c"
         "src/fck_rescue.c"
         "src/fck_wifi.c"
         "src/fck_gnss.c"
         "src/fck_subghz.c"
    INCLUDE_DIRS "include" "core/include"
    REQUIRES fck_bsp
    PRIV_REQUIRES fck_aos kode freertos
)
# fck_core.h is for this component and fck_sys alone.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FCK_CORE_ALLOWED)
