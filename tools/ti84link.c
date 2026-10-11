/*
 * ti84link - talk to a TI-84 Plus / Plus SE running TI-OS over USB
 * (libticalcs from the tilibs project).
 *
 *   ti84link info               show OS/boot versions
 *   ti84link backup DIR         save every variable and Flash app into DIR
 *   ti84link romdump FILE       dump the whole flash (takes several minutes)
 *   ti84link check FILE.8xu     parse an OS upgrade (no calculator needed)
 *   ti84link sendos FILE.8xu    send an OS upgrade (refused for boot code
 *                               1.03 and newer unless --force is given)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <glib.h>
#include <ticonv.h>
#include <tifiles.h>
#include <ticables.h>
#include <ticalcs.h>

static CableHandle *cable;
static CalcHandle *calc;
static CalcModel model;
static int is_se;

static int fail(const char *what, int err)
{
    char *msg = NULL;
    ticalcs_error_get(err, &msg);
    if (!msg)
        ticables_error_get(err, &msg);
    fprintf(stderr, "ti84link: %s failed: %d %s\n", what, err, msg ? msg : "");
    g_free(msg);
    return 1;
}

static int open_calc(void)
{
    int *pids = NULL, n = 0, err;
    ticables_library_init();
    tifiles_library_init();
    ticalcs_library_init();
    ticables_get_usb_devices(&pids, &n);
    if (n < 1) {
        fprintf(stderr, "ti84link: no TI calculator found on USB (is it connected and on?)\n");
        return 1;
    }
    if (pids[0] == PID_TI84P)
        model = CALC_TI84P_USB;
    else if (pids[0] == PID_TI84P_SE) {
        model = CALC_TI84P_USB;          /* same protocol */
        is_se = 1;
    }
    else {
        fprintf(stderr, "ti84link: USB device %04x is not a TI-84 Plus\n", pids[0]);
        return 1;
    }
    cable = ticables_handle_new(CABLE_USB, PORT_1);
    ticables_options_set_timeout(cable, 50);   /* 5 s */
    calc = ticalcs_handle_new(model);
    if ((err = ticalcs_cable_attach(calc, cable)))
        return fail("cable attach", err);
    if ((err = ticalcs_calc_isready(calc)))
        return fail("ready check", err);
    return 0;
}

static void close_calc(void)
{
    if (calc) {
        ticalcs_cable_detach(calc);
        ticalcs_handle_del(calc);
    }
    if (cable)
        ticables_handle_del(cable);
}

static int get_info(CalcInfos *ci)
{
    int err;
    memset(ci, 0, sizeof(*ci));
    if ((err = ticalcs_calc_get_version(calc, ci)))
        return fail("get version", err);
    return 0;
}

static int boot_is_103_or_newer(const char *v)
{
    int major = 0, minor = 0;
    if (sscanf(v, "%d.%d", &major, &minor) != 2)
        return 1;                        /* unknown: be careful */
    return major > 1 || (major == 1 && minor >= 3);
}

static int cmd_info(void)
{
    CalcInfos ci;
    if (get_info(&ci))
        return 1;
    printf("model:   %s\n", is_se ? "84pse" : "84p");
    printf("product: %s\n", ci.product_name);
    printf("OS:      %s\n", ci.os_version);
    printf("boot:    %s\n", ci.boot_version);
    printf("hw:      %u\n", ci.hw_version);
    printf("ID:      %s\n", ci.main_calc_id);
    return 0;
}

static int recv_tree(GNode *tree, const char *dir, int apps)
{
    int failures = 0;
    for (unsigned i = 0; tree && i < g_node_n_children(tree); i++) {
        GNode *folder = g_node_nth_child(tree, i);
        for (unsigned j = 0; j < g_node_n_children(folder); j++) {
            VarEntry *ve = (VarEntry *)g_node_nth_child(folder, j)->data;
            char *name = tifiles_build_filename(model, ve);
            char *path = g_build_filename(dir, name, NULL);
            int err;
            printf("  %s ... ", name);
            fflush(stdout);
            if (apps)
                err = ticalcs_calc_recv_app2(calc, path, ve);
            else
                err = ticalcs_calc_recv_var2(calc, MODE_NORMAL, path, ve);
            if (err) {
                printf("FAILED (%d)\n", err);
                failures++;
            } else {
                printf("ok\n");
            }
            g_free(path);
            g_free(name);
        }
    }
    return failures;
}

static int cmd_backup(const char *dir)
{
    GNode *vars = NULL, *apps = NULL;
    int err, failures;
    CalcInfos ci;
    FILE *f;
    char *p;
    if (get_info(&ci))
        return 1;
    g_mkdir_with_parents(dir, 0755);
    p = g_build_filename(dir, "calc-info.txt", NULL);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "product %s\nOS %s\nboot %s\nhw %u\nID %s\n", ci.product_name, ci.os_version,
                ci.boot_version, ci.hw_version, ci.main_calc_id);
        fclose(f);
    }
    g_free(p);
    if ((err = ticalcs_calc_get_dirlist(calc, &vars, &apps)))
        return fail("directory listing", err);
    printf("%u variables, %u apps\n", ticalcs_dirlist_ve_count(vars), ticalcs_dirlist_ve_count(apps));
    failures = recv_tree(vars, dir, 0);
    failures += recv_tree(apps, dir, 1);
    ticalcs_dirlist_destroy(&vars);
    ticalcs_dirlist_destroy(&apps);
    if (failures) {
        fprintf(stderr, "ti84link: %d item(s) could not be saved\n", failures);
        return 1;
    }
    printf("backup complete: %s\n", dir);
    return 0;
}

static int cmd_romdump(const char *file)
{
    int err;
    printf("sending the dumper, keep the calculator on the home screen...\n");
    if ((err = ticalcs_calc_dump_rom_1(calc)))
        return fail("dumper upload", err);
    printf("dumping (several minutes)...\n");
    if ((err = ticalcs_calc_dump_rom_2(calc, ROMSIZE_AUTO, file)))
        return fail("ROM dump", err);
    printf("ROM saved to %s\n", file);
    return 0;
}

static int cmd_sendos(const char *file, int force)
{
    CalcInfos ci;
    int err;
    if (get_info(&ci))
        return 1;
    printf("calculator: OS %s, boot %s\n", ci.os_version, ci.boot_version);
    if (boot_is_103_or_newer(ci.boot_version) && !force) {
        fprintf(stderr, "ti84link: boot code %s rejects third-party OS upgrades;\n"
                        "  use UOSRECV (see ti84p/README.md) or --force\n", ci.boot_version);
        return 2;
    }
    printf("sending %s (do not disconnect)...\n", file);
    if ((err = ticalcs_calc_send_os2(calc, file)))
        return fail("OS transfer", err);
    printf("OS sent\n");
    return 0;
}

/* parse an OS/app file like TiLP would, without a calculator */
static int cmd_check(const char *file)
{
    FlashContent *fc;
    int err;
    tifiles_library_init();
    fc = tifiles_content_create_flash(CALC_TI84P);
    if ((err = tifiles_file_read_flash(file, fc))) {
        fprintf(stderr, "ti84link: %s: cannot parse (%d)\n", file, err);
        return 1;
    }
    for (FlashContent *p = fc; p; p = p->next) {
        unsigned total = 0;
        printf("type %02x, %u pages, %u data bytes\n", p->data_type, p->num_pages, p->data_length);
        for (unsigned i = 0; i < p->num_pages; i++) {
            FlashPage *fp = p->pages[i];
            total += fp->size;
            if (i < 3 || i + 3 > p->num_pages || fp->size != 0x4000)
                printf("  #%u: page %02x addr %04x flag %02x size %u\n", i, fp->page, fp->addr,
                       fp->flag, fp->size);
        }
        printf("  total %u bytes\n", total);
    }
    tifiles_content_delete_flash(fc);
    return 0;
}

int main(int argc, char **argv)
{
    int rc;
    if (argc > 2 && !strcmp(argv[1], "check"))
        return cmd_check(argv[2]);
    if (argc < 2) {
        fprintf(stderr, "usage: ti84link info | backup DIR | romdump FILE | sendos FILE.8xu [--force]\n");
        return 2;
    }
    if (open_calc()) {
        close_calc();
        return 1;
    }
    if (!strcmp(argv[1], "info"))
        rc = cmd_info();
    else if (!strcmp(argv[1], "backup") && argc > 2)
        rc = cmd_backup(argv[2]);
    else if (!strcmp(argv[1], "romdump") && argc > 2)
        rc = cmd_romdump(argv[2]);
    else if (!strcmp(argv[1], "sendos") && argc > 2)
        rc = cmd_sendos(argv[2], argc > 3 && !strcmp(argv[3], "--force"));
    else {
        fprintf(stderr, "ti84link: bad command\n");
        rc = 2;
    }
    close_calc();
    return rc;
}
