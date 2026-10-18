/* Generated from public tools/runtime_schema.py. */
static void decode_MiBandNandGeometry(QObject *obj, MiBandNandGeometry *p)
{
    QDict *d = read_object(obj, 5);
    p->page_size = read_int(required(d, "page_size"), 2048LL, 2048LL);
    p->spare_size = read_int(required(d, "spare_size"), 64LL, 128LL);
    p->pages_per_block = read_int(required(d, "pages_per_block"), 64LL, 64LL);
    p->page_count = read_int(required(d, "page_count"), 1LL, 262144LL);
    { QList *a0 = read_array(required(d, "jedec_id"), 3, 3);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->jedec_id[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
}
static void decode_MiBandBoardConfig(QObject *obj, MiBandBoardConfig *p)
{
    QDict *d = read_object(obj, 24);
    p->id = read_str(required(d, "id"));
    p->width = read_int(required(d, "width"), 1LL, 1024LL);
    p->height = read_int(required(d, "height"), 1LL, 1024LL);
    p->framebuffer_bpp = read_int(required(d, "framebuffer_bpp"), 16LL, 32LL);
    p->charger_bus = read_int(required(d, "charger_bus"), 0LL, 5LL);
    p->panel_te_gpio = read_int(required(d, "panel_te_gpio"), 0LL, 95LL);
    p->battery_channel = read_int(required(d, "battery_channel"), 0LL, 6LL);
    p->thermistor_mask = read_int(required(d, "thermistor_mask"), 1LL, 127LL);
    p->thermistor_mv = read_int(required(d, "thermistor_mv"), 0LL, 1800LL);
    p->battery_divider_uv_per_mv = read_int(required(d, "battery_divider_uv_per_mv"), 1000LL, 10000LL);
    { QList *a0 = read_array(required(d, "curve_uv"), 101, 101);
    uint32_t *items = g_new0(uint32_t, qlist_size(a0));
    p->curve_uv = items;
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    items[i0] = read_int(e0->value, 2500000LL, 5000000LL);
    i0++;
    } }
    p->touch_model = read_str(required(d, "touch_model"));
    p->touch_bus = read_int(required(d, "touch_bus"), 0LL, 7LL);
    p->touch_address = read_int(required(d, "touch_address"), 1LL, 127LL);
    p->touch_irq_gpio = read_int(required(d, "touch_irq_gpio"), 0LL, 95LL);
    p->touch_chip_id = read_int(required(d, "touch_chip_id"), 1LL, 65535LL);
    p->panel_id = read_int(required(d, "panel_id"), 0LL, 16777215LL);
    p->config_sha256 = read_hash(required(d, "config_sha256"));
    p->nand_bus = read_int(required(d, "nand_bus"), 1LL, 1LL);
    decode_MiBandNandGeometry(required(d, "nand"), &p->nand);
    p->charger_probe_revision = read_int(required(d, "charger_probe_revision"), 0LL, 255LL);
    p->touch_sleep_gesture = read_int(required(d, "touch_sleep_gesture"), 0LL, 1LL);
    p->touch_double_tap_status = read_int(required(d, "touch_double_tap_status"), 1LL, 2LL);
    p->touch_reset_gpio = read_int(required(d, "touch_reset_gpio"), -1LL, 95LL);
}
static void decode_MiBandFirmwareConfig(QObject *obj, MiBandFirmwareConfig *p)
{
    QDict *d = read_object(obj, 24);
    p->id = read_str(required(d, "id"));
    p->board = read_str(required(d, "board"));
    p->config_sha256 = read_hash(required(d, "config_sha256"));
    p->storage_sha256 = read_hash(required(d, "storage_sha256"));
    p->ble_service_uuid = read_str(required(d, "ble_service_uuid"));
    p->ble_tx_uuid = read_str(required(d, "ble_tx_uuid"));
    p->ble_rx_uuid = read_str(required(d, "ble_rx_uuid"));
    p->miwear_service_uuid = read_str(required(d, "miwear_service_uuid"));
    p->gattc_service_uuid = read_str(required(d, "gattc_service_uuid"));
    p->boot_sp = read_int(required(d, "boot_sp"), 0LL, 4294967295LL);
    p->boot_pc = read_int(required(d, "boot_pc"), 0LL, 4294967295LL);
    p->nor_address = read_int(required(d, "nor_address"), 0LL, 16777215LL);
    p->nor_image_offset = read_int(required(d, "nor_image_offset"), -16777216LL, 16777216LL);
    p->nor_size = read_int(required(d, "nor_size"), 1LL, 16777216LL);
    p->resource_first_page = read_int(required(d, "resource_first_page"), 0LL, 262143LL);
    p->resource_page_count = read_int(required(d, "resource_page_count"), 1LL, 262144LL);
    p->ble_mtu = read_int(required(d, "ble_mtu"), 23LL, 65535LL);
    p->ble_chunk_size = read_int(required(d, "ble_chunk_size"), 1LL, 65532LL);
    p->mass_slice_length = read_int(required(d, "mass_slice_length"), 1LL, 4294967295LL);
    { QList *a0 = read_array(required(d, "mass_types"), 1, 32);
    uint8_t *items = g_new0(uint8_t, qlist_size(a0));
    p->mass_types = items;
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    items[i0] = read_int(e0->value, 1LL, 255LL);
    i0++;
    } }
    p->mass_type_count = read_int(required(d, "mass_type_count"), 1LL, 32LL);
    p->flash_offset = read_int(required(d, "flash_offset"), 0LL, 16777215LL);
    p->resource_format = read_str(required(d, "resource_format"));
    p->legacy_journal = read_bool(required(d, "legacy_journal"));
    require(qlist_size(qdict_get_qlist(d, "mass_types")) == p->mass_type_count, "mass type count");
}
static void decode_MiBandAudioHook(QObject *obj, MiBandAudioHook *p)
{
    QDict *d = read_object(obj, 3);
    p->address = read_int(required(d, "address"), 0LL, 4294967295LL);
    p->entry = read_int(required(d, "entry"), 8LL, 36LL);
    { QList *a0 = read_array(required(d, "expected"), 8, 8);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->expected[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
}
static void decode_MiBandAudioProfile(QObject *obj, MiBandAudioProfile *p)
{
    QDict *d = read_object(obj, 8);
    p->id = read_str(required(d, "id"));
    p->ap_sha256 = read_hash(required(d, "ap_sha256"));
    p->config_sha256 = read_hash(required(d, "config_sha256"));
    p->flash_base = read_int(required(d, "flash_base"), 0LL, 4294967295LL);
    p->ram_base = read_int(required(d, "ram_base"), 0LL, 4294967295LL);
    p->mmio_base = read_int(required(d, "mmio_base"), 0LL, 4294967295LL);
    p->legacy_abi2 = read_bool(required(d, "legacy_abi2"));
    { QList *a0 = read_array(required(d, "hooks"), 7, 7);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    decode_MiBandAudioHook(e0->value, &p->hooks[i0]);
    i0++;
    } }
}
static void decode_MiBandBindingUiPatch(QObject *obj, MiBandBindingUiPatch *p)
{
    QDict *d = read_object(obj, 7);
    p->address = read_int(required(d, "address"), 0LL, 4294967295LL);
    p->guard_address = read_int(required(d, "guard_address"), 0LL, 4294967295LL);
    p->patch_size = read_int(required(d, "patch_size"), 1LL, 8LL);
    p->guard_size = read_int(required(d, "guard_size"), 1LL, 32LL);
    { QList *a0 = read_array(required(d, "expected"), 8, 8);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->expected[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
    { QList *a0 = read_array(required(d, "replacement"), 8, 8);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->replacement[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
    { QList *a0 = read_array(required(d, "guard"), 32, 32);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->guard[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
}
static void decode_MiBandBindingAuthPatch(QObject *obj, MiBandBindingAuthPatch *p)
{
    QDict *d = read_object(obj, 7);
    p->address = read_int(required(d, "address"), 0LL, 4294967295LL);
    p->guard_address = read_int(required(d, "guard_address"), 0LL, 4294967295LL);
    p->patch_size = read_int(required(d, "patch_size"), 1LL, 8LL);
    p->guard_size = read_int(required(d, "guard_size"), 1LL, 32LL);
    { QList *a0 = read_array(required(d, "expected"), 8, 8);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->expected[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
    { QList *a0 = read_array(required(d, "replacement"), 8, 8);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->replacement[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
    { QList *a0 = read_array(required(d, "guard"), 32, 32);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    p->guard[i0] = read_int(e0->value, 0LL, 255LL);
    i0++;
    } }
}
static void decode_MiBandBindingUiProfile(QObject *obj, MiBandBindingUiProfile *p)
{
    QDict *d = read_object(obj, 6);
    p->id = read_str(required(d, "id"));
    p->ap_sha256 = read_hash(required(d, "ap_sha256"));
    p->config_sha256 = read_hash(required(d, "config_sha256"));
    p->flash_base = read_int(required(d, "flash_base"), 0LL, 4294967295LL);
    p->patch_count = read_int(required(d, "patch_count"), 1LL, 2LL);
    { QList *a0 = read_array(required(d, "patches"), 1, 2);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    decode_MiBandBindingUiPatch(e0->value, &p->patches[i0]);
    i0++;
    } }
    require(qlist_size(qdict_get_qlist(d, "patches")) == p->patch_count, "patch count");
}
static void decode_MiBandBindingAuthProfile(QObject *obj, MiBandBindingAuthProfile *p)
{
    QDict *d = read_object(obj, 6);
    p->id = read_str(required(d, "id"));
    p->ap_sha256 = read_hash(required(d, "ap_sha256"));
    p->config_sha256 = read_hash(required(d, "config_sha256"));
    p->flash_base = read_int(required(d, "flash_base"), 0LL, 4294967295LL);
    p->patch_count = read_int(required(d, "patch_count"), 1LL, 8LL);
    { QList *a0 = read_array(required(d, "patches"), 1, 8);
    size_t i0 = 0; const QListEntry *e0;
    QLIST_FOREACH_ENTRY(a0, e0) {
    decode_MiBandBindingAuthPatch(e0->value, &p->patches[i0]);
    i0++;
    } }
    require(qlist_size(qdict_get_qlist(d, "patches")) == p->patch_count, "patch count");
}
