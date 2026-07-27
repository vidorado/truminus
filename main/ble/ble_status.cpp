#include "ble_status.hpp"
#include "victronble.hpp"
#include "ultimatronble.hpp"
#include "tankble.hpp"
#include "multiplusble.hpp"
#include "openairble.hpp"

int bleIconState() {
    if (victronGetData().valid || ultimatronGetData().valid ||
        tankGetData().valid    || multiplusGetData().valid  ||
        openairGetData().valid)
        return 2;
    if (victronIsConfigured() || ultimatronIsConfigured() ||
        tankIsConfigured()    || multiplusIsConfigured()  ||
        openairIsConfigured())
        return 1;
    return 0;
}
