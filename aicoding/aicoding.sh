#!/bin/sh
# aicoding wrapper - sets runtime library path and forwards all arguments.
export LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
exec /opt/my_db/aicoding/aicoding "$@"
