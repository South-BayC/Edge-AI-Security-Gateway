/**
 * error_code.c — 统一错误码可读化（B4）
 */
#include "error_code.h"

#include <stdio.h>

typedef struct {
    int code;
    const char *name;
} err_entry_t;

static const err_entry_t k_errs[] = {
    { ERR_CFG_PATH,      "CFG:PATH"      },
    { ERR_CFG_PARSE,     "CFG:PARSE"     },
    { ERR_CFG_WRITE,     "CFG:WRITE"     },

    { ERR_LOG_INIT,      "LOG:INIT"      },
    { ERR_LOG_WRITE,     "LOG:WRITE"     },

    { ERR_SENSOR_COMM,   "SENSOR:COMM"   },
    { ERR_SENSOR_STALE,  "SENSOR:STALE"  },
    { ERR_SENSOR_RANGE,  "SENSOR:RANGE"  },
    { ERR_SENSOR_SPIKE,  "SENSOR:SPIKE"  },

    { ERR_CAM_OPEN,      "CAM:OPEN"      },
    { ERR_CAM_FMT,       "CAM:FMT"       },
    { ERR_CAM_STREAM,    "CAM:STREAM"    },
    { ERR_CAM_FRAME,     "CAM:FRAME"     },

    { ERR_DET_QUEUE,     "DET:QUEUE"     },
    { ERR_DET_NO_FRAME,  "DET:NO_FRAME"  },
    { ERR_DET_MODEL,     "DET:MODEL"     },

    { ERR_MQTT_CONNECT,  "MQTT:CONNECT"  },
    { ERR_MQTT_AUTH,     "MQTT:AUTH"     },
    { ERR_MQTT_SEND,     "MQTT:SEND"     },
    { ERR_MQTT_RECV,     "MQTT:RECV"     },

    { ERR_OTA_MANIFEST,  "OTA:MANIFEST"  },
    { ERR_OTA_FETCH,     "OTA:FETCH"     },
    { ERR_OTA_SHA,       "OTA:SHA"       },
    { ERR_OTA_HMAC,      "OTA:HMAC"      },
    { ERR_OTA_EXTRACT,   "OTA:EXTRACT"   },
    { ERR_OTA_INSTALL,   "OTA:INSTALL"   },

    { ERR_TIME_RESOLVE,  "TIME:RESOLVE"  },
    { ERR_TIME_RECV,     "TIME:RECV"     },
    { ERR_TIME_SET,      "TIME:SET"      },

    { ERR_BRIDGE_IIO,    "BRIDGE:IIO"    },
    { ERR_BRIDGE_KEY,    "BRIDGE:KEY"    },

    { ERR_CLOUD_CONNECT, "CLOUD:CONNECT" },
    { ERR_CLOUD_STATUS,  "CLOUD:STATUS"  },
    { ERR_CLOUD_AUTH,    "CLOUD:AUTH"    },
};

const char *err_str(int code)
{
    static char unknown[24];

    if (code == 0)
        return "OK";

    for (unsigned i = 0; i < sizeof(k_errs) / sizeof(k_errs[0]); i++) {
        if (k_errs[i].code == code)
            return k_errs[i].name;
    }

    snprintf(unknown, sizeof(unknown), "ERR(%d)", code);
    return unknown;
}
