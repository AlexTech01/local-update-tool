#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Official Ounce 23.0.1 LRP defaults and layouts. */
enum { LabAdvertisementSize=0x100, LabRoomProtocol=0x13, LabApplicationSummaryBytes=0x80 };
#define LAB_COMMUNICATION_ID UINT64_C(0x0100000000001000)
#define LAB_SCENE_ID 1

bool labBuildApplicationSummary(uint8_t out[LabApplicationSummaryBytes], uint64_t application_id,
                               const void *control, size_t control_size, uint64_t estimated_size);
bool labBuildHostAdvertisement(uint8_t out[LabAdvertisementSize], const uint8_t summary[LabApplicationSummaryBytes],
                               uint32_t room_nonce, unsigned max_players, const char *nickname);
unsigned labDeliveryProtocol(const void *records, unsigned count);

/* LRP +0xA2B60 updates size/display version after selecting the canonical patch. */
void labUpdateHostApplication(uint8_t advertise[LabAdvertisementSize],
    const uint8_t summary[LabApplicationSummaryBytes]);
/* LRP +0xA3680: status bytes, padding, then big-endian participant ID. */
void labUpdateHostProgress(uint8_t advertise[LabAdvertisementSize], uint32_t host_node,
    uint32_t peer_node, bool started, bool receiving, bool complete);
