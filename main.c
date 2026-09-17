#include <stdio.h>

#include "app.h"
#include "cli.h"
#include "console.h"
#include "instance.h"
#include "ui.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    cli_install_signals();

    /* if (argc > 1) {
        return cli_run(argc, argv);
    } */
    (void)argc;
    (void)argv;

    /* A second launch hands its request to the copy already running, which
       raises its window; there is nothing left for this one to do. */
    if (!instance_acquire()) {
        return 0;
    }

    static app_t app;
    app_init(&app);

    /* app_init has read config.json by now, so "debug" is known. */
    console_init(app.opts.debug);

    app_connect(&app);

    int status = ui_run(&app);
    app_shutdown(&app);
    instance_release();
    console_shutdown();
    return status;
}
