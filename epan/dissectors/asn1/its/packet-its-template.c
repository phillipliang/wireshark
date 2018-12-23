/* packet-its-template.c
 *
 * Intelligent Transport Systems Applications dissectors
 * Coyright 2018, C. Guerber <cguerber@yahoo.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Implemented:
 * CA (CAM)                           ETSI EN 302 637-2
 * DEN (DENM)                         ETSI EN 302 637-3
 * RLT (MAPEM)                        ETSI TS 103 301
 * TLM (SPATEM)                       ETSI TS 103 301
 * IVI (IVIM)                         ETSI TS 103 301
 * TLC (SREM)                         ETSI TS 103 301
 * TLC (SSEM)                         ETSI TS 103 301
 * EVCSN POI (EVCSN POI message)      ETSI TS 101 556-1
 * TPG (TRM, TCM, VDRM, VDPM, EOFM)   ETSI TS 101 556-2
 * Charging (EV-RSR, SRM, SCM)        ETSI TS 101 556-3
 *
 * Not supported:
 * SA (SAEM)                          ETSI TS 102 890-1
 * GPC (RTCMEM)                       ETSI TS 103 301
 * CTL (CTLM)                         ETSI TS 102 941
 * CRL (CRLM)                         ETSI TS 102 941
 * Certificate request                ETSI TS 102 941
 */
#include "config.h"

#include <epan/packet.h>
#include <epan/expert.h>
#include <epan/decode_as.h>
#include <epan/proto_data.h>
#include <epan/exceptions.h>
#include <epan/conversation.h>
#include <epan/tap.h>
#include <wsutil/utf8_entities.h>
#include "packet-ber.h"
#include "packet-per.h"

#include "packet-its.h"

/*
 * Well Known Ports definitions as per:
 *
 * ETSI TS 103 248 v1.2.1 (2018-08)
 * Intelligent Transport Systems (ITS);
 * GeoNetworking;
 * Port Numbers for the Basic Transport Protocol (BTP)
 *
 * BTP port   Facilities service      Related standard
 * number     or Application
 * values
 * 2001       CA (CAM)                ETSI EN 302 637-2
 * 2002       DEN (DENM)              ETSI EN 302 637-3
 * 2003       RLT (MAPEM)             ETSI TS 103 301
 * 2004       TLM (SPATEM)            ETSI TS 103 301
 * 2005       SA (SAEM)               ETSI TS 102 890-1
 * 2006       IVI (IVIM)              ETSI TS 103 301
 * 2007       TLC (SREM)              ETSI TS 103 301
 * 2008       TLC (SSEM)              ETSI TS 103 301
 * 2009       Allocated               Allocated for "Intelligent Transport
 *                                    System (ITS); Vehicular Communications;
 *                                    Basic Set of Applications; Specification
 *                                    of the Collective Perception Service"
 * 2010       EVCSN POI (EVCSN POI    ETSI TS 101 556-1
 *            message)
 * 2011       TPG (TRM, TCM, VDRM,    ETSI TS 101 556-2
 *            VDPM, EOFM)
 * 2012       Charging (EV-RSR,       ETSI TS 101 556-3
 *            SRM, SCM)
 * 2013       GPC (RTCMEM)            ETSI TS 103 301
 * 2014       CTL (CTLM)              ETSI TS 102 941
 * 2015       CRL (CRLM)              ETSI TS 102 941
 * 2016       Certificate request     ETSI TS 102 941
 */

// Applications Well Known Ports
#define ITS_WKP_CA         2001
#define ITS_WKP_DEN        2002
#define ITS_WKP_RLT        2003
#define ITS_WKP_TLM        2004
#define ITS_WKP_SA         2005
#define ITS_WKP_IVI        2006
#define ITS_WKP_TLC_SREM   2007
#define ITS_WKP_TLC_SSEM   2008
#define ITS_WKP_CPS        2009
#define ITS_WKP_EVCSN      2010
#define ITS_WKP_TPG        2011
#define ITS_WKP_CHARGING   2012
#define ITS_WKP_GPC        2013
#define ITS_WKP_CTL        2014
#define ITS_WKP_CRL        2015
#define ITS_WKP_CERTIF_REQ 2016

// ETSI TS 102 965 (V1.3.1)
#define AID_CA       36
#define AID_DEN      37
#define AID_TLM     137
#define AID_RLT     138
#define AID_IVI     139
#define AID_TLC     140
#define AID_GN_MGMT 141

/*
 * Prototypes
 */
void proto_reg_handoff_its(void);
void proto_register_its(void);

// TAP
static int its_tap = -1;

// Protocols
static int proto_its = -1;
static int proto_its_denm = -1;
static int proto_its_cam = -1;
static int proto_its_evcsn = -1;
static int proto_its_evrsr = -1;
static int proto_its_ivim = -1;
static int proto_its_tistpg = -1;
static int proto_its_ssem = -1;
static int proto_its_srem = -1;
static int proto_its_mapem = -1;
static int proto_its_spatem = -1;
static int proto_addgrpc = -1;

// Subdissectors
static dissector_table_t its_version_subdissector_table;
static dissector_table_t its_msgid_subdissector_table;
static dissector_table_t regionid_subdissector_table;

typedef struct its_private_data {
    enum regext_type_enum type;
    guint32 region_id;
    guint32 cause_code;
} its_private_data_t;

// Specidic dissector for content of open type for regional extensions
static int dissect_regextval_pdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
    its_private_data_t *re = (its_private_data_t*)data;
    // XXX What to do when region_id = noRegion? Test length is zero?
    if (!dissector_try_uint_new(regionid_subdissector_table, ((guint32) re->region_id<<16) + (guint32) re->type, tvb, pinfo, tree, FALSE, NULL))
        call_data_dissector(tvb, pinfo, tree);
    return tvb_captured_length(tvb);
}

// Generated by asn2wrs
#include "packet-its-hf.c"

// CauseCode/SubCauseCode management
static int hf_its_trafficConditionSubCauseCode = -1;
static int hf_its_accidentSubCauseCode = -1;
static int hf_its_roadworksSubCauseCode = -1;
static int hf_its_adverseWeatherCondition_PrecipitationSubCauseCode = -1;
static int hf_its_adverseWeatherCondition_VisibilitySubCauseCode = -1;
static int hf_its_adverseWeatherCondition_AdhesionSubCauseCode = -1;
static int hf_its_adverseWeatherCondition_ExtremeWeatherConditionSubCauseCode = -1;
static int hf_its_hazardousLocation_AnimalOnTheRoadSubCauseCode = -1;
static int hf_its_hazardousLocation_ObstacleOnTheRoadSubCauseCode = -1;
static int hf_its_hazardousLocation_SurfaceConditionSubCauseCode = -1;
static int hf_its_hazardousLocation_DangerousCurveSubCauseCode = -1;
static int hf_its_humanPresenceOnTheRoadSubCauseCode = -1;
static int hf_its_wrongWayDrivingSubCauseCode = -1;
static int hf_its_rescueAndRecoveryWorkInProgressSubCauseCode = -1;
static int hf_its_slowVehicleSubCauseCode = -1;
static int hf_its_dangerousEndOfQueueSubCauseCode = -1;
static int hf_its_vehicleBreakdownSubCauseCode = -1;
static int hf_its_postCrashSubCauseCode = -1;
static int hf_its_humanProblemSubCauseCode = -1;
static int hf_its_stationaryVehicleSubCauseCode = -1;
static int hf_its_emergencyVehicleApproachingSubCauseCode = -1;
static int hf_its_collisionRiskSubCauseCode = -1;
static int hf_its_signalViolationSubCauseCode = -1;
static int hf_its_dangerousSituationSubCauseCode = -1;

static gint ett_its = -1;

#include "packet-its-ett.c"

// Deal with cause/subcause code management
struct { CauseCodeType_enum cause; int* hf; } cause_to_subcause[] = {
    { trafficCondition, &hf_its_trafficConditionSubCauseCode },
    { accident, &hf_its_accidentSubCauseCode },
    { roadworks, &hf_its_roadworksSubCauseCode },
    { adverseWeatherCondition_Precipitation, &hf_its_adverseWeatherCondition_PrecipitationSubCauseCode },
    { adverseWeatherCondition_Visibility, &hf_its_adverseWeatherCondition_VisibilitySubCauseCode },
    { adverseWeatherCondition_Adhesion, &hf_its_adverseWeatherCondition_AdhesionSubCauseCode },
    { adverseWeatherCondition_ExtremeWeatherCondition, &hf_its_adverseWeatherCondition_ExtremeWeatherConditionSubCauseCode },
    { hazardousLocation_AnimalOnTheRoad, &hf_its_hazardousLocation_AnimalOnTheRoadSubCauseCode },
    { hazardousLocation_ObstacleOnTheRoad, &hf_its_hazardousLocation_ObstacleOnTheRoadSubCauseCode },
    { hazardousLocation_SurfaceCondition, &hf_its_hazardousLocation_SurfaceConditionSubCauseCode },
    { hazardousLocation_DangerousCurve, &hf_its_hazardousLocation_DangerousCurveSubCauseCode },
    { humanPresenceOnTheRoad, &hf_its_humanPresenceOnTheRoadSubCauseCode },
    { wrongWayDriving, &hf_its_wrongWayDrivingSubCauseCode },
    { rescueAndRecoveryWorkInProgress, &hf_its_rescueAndRecoveryWorkInProgressSubCauseCode },
    { slowVehicle, &hf_its_slowVehicleSubCauseCode },
    { dangerousEndOfQueue, &hf_its_dangerousEndOfQueueSubCauseCode },
    { vehicleBreakdown, &hf_its_vehicleBreakdownSubCauseCode },
    { postCrash, &hf_its_postCrashSubCauseCode },
    { humanProblem, &hf_its_humanProblemSubCauseCode },
    { stationaryVehicle, &hf_its_stationaryVehicleSubCauseCode },
    { emergencyVehicleApproaching, &hf_its_emergencyVehicleApproachingSubCauseCode },
    { collisionRisk, &hf_its_collisionRiskSubCauseCode },
    { signalViolation, &hf_its_signalViolationSubCauseCode },
    { dangerousSituation, &hf_its_dangerousSituationSubCauseCode },
    { reserved, NULL },
};

static int*
find_subcause_from_cause(CauseCodeType_enum cause)
{
    int idx = 0;

    while (cause_to_subcause[idx].hf && (cause_to_subcause[idx].cause != cause))
        idx++;

    return cause_to_subcause[idx].hf?cause_to_subcause[idx].hf:&hf_its_subCauseCode;
}

#include "packet-its-fn.c"

static int
dissect_its_PDU(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
  proto_item *its_item;
  proto_tree *its_tree;

  col_set_str(pinfo->cinfo, COL_PROTOCOL, "ITS");
  col_clear(pinfo->cinfo, COL_INFO);

  its_item = proto_tree_add_item(tree, proto_its, tvb, 0, -1, ENC_NA);
  its_tree = proto_item_add_subtree(its_item, ett_its);

  return dissect_its_ItsPduHeader_PDU(tvb, pinfo, its_tree, data);
}

// Decode As...
static void
its_msgid_prompt(packet_info *pinfo, gchar *result)
{
    guint32 msgid = GPOINTER_TO_UINT(p_get_proto_data(pinfo->pool, pinfo, hf_its_messageID, pinfo->curr_layer_num));

    g_snprintf(result, MAX_DECODE_AS_PROMPT_LEN, "MsgId (%s%u)", UTF8_RIGHTWARDS_ARROW, msgid);
}

static gpointer
its_msgid_value(packet_info *pinfo)
{
    return p_get_proto_data(pinfo->pool, pinfo, hf_its_messageID, pinfo->curr_layer_num);
}

// Registration of protocols
void proto_register_its(void)
{
    static hf_register_info hf_its[] = {
        #include "packet-its-hfarr.c"

    { &hf_its_roadworksSubCauseCode,
      { "roadworksSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_RoadworksSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_postCrashSubCauseCode,
      { "postCrashSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_PostCrashSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_vehicleBreakdownSubCauseCode,
      { "vehicleBreakdownSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_VehicleBreakdownSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_dangerousSituationSubCauseCode,
      { "dangerousSituationSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_DangerousSituationSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_dangerousEndOfQueueSubCauseCode,
      { "dangerousEndOfQueueSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_DangerousEndOfQueueSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_rescueAndRecoveryWorkInProgressSubCauseCode,
      { "rescueAndRecoveryWorkInProgressSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_RescueAndRecoveryWorkInProgressSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_signalViolationSubCauseCode,
      { "signalViolationSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_SignalViolationSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_collisionRiskSubCauseCode,
      { "collisionRiskSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_CollisionRiskSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_hazardousLocation_AnimalOnTheRoadSubCauseCode,
      { "hazardousLocation_AnimalOnTheRoadSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_HazardousLocation_AnimalOnTheRoadSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_hazardousLocation_ObstacleOnTheRoadSubCauseCode,
      { "hazardousLocation_ObstacleOnTheRoadSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_HazardousLocation_ObstacleOnTheRoadSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_hazardousLocation_SurfaceConditionSubCauseCode,
      { "hazardousLocation_SurfaceConditionSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_HazardousLocation_SurfaceConditionSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_hazardousLocation_DangerousCurveSubCauseCode,
      { "hazardousLocation_DangerousCurveSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_HazardousLocation_DangerousCurveSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_emergencyVehicleApproachingSubCauseCode,
      { "emergencyVehicleApproachingSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_EmergencyVehicleApproachingSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_humanProblemSubCauseCode,
      { "humanProblemSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_HumanProblemSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_stationaryVehicleSubCauseCode,
      { "stationaryVehicleSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_StationaryVehicleSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_slowVehicleSubCauseCode,
      { "slowVehicleSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_SlowVehicleSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_adverseWeatherCondition_PrecipitationSubCauseCode,
      { "adverseWeatherCondition_PrecipitationSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_AdverseWeatherCondition_PrecipitationSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_adverseWeatherCondition_VisibilitySubCauseCode,
      { "adverseWeatherCondition_VisibilitySubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_AdverseWeatherCondition_VisibilitySubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_adverseWeatherCondition_AdhesionSubCauseCode,
      { "adverseWeatherCondition_AdhesionSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_AdverseWeatherCondition_AdhesionSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_adverseWeatherCondition_ExtremeWeatherConditionSubCauseCode,
      { "adverseWeatherCondition_ExtremeWeatherConditionSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_AdverseWeatherCondition_ExtremeWeatherConditionSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_wrongWayDrivingSubCauseCode,
      { "wrongWayDrivingSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_WrongWayDrivingSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_humanPresenceOnTheRoadSubCauseCode,
      { "humanPresenceOnTheRoadSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_HumanPresenceOnTheRoadSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_accidentSubCauseCode,
      { "accidentSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_AccidentSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    { &hf_its_trafficConditionSubCauseCode,
      { "trafficConditionSubCauseCode", "its.subCauseCode",
        FT_UINT32, BASE_DEC, VALS(its_TrafficConditionSubCauseCode_vals), 0,
        "SubCauseCodeType", HFILL }},
    };

    static gint *ett[] = {
        &ett_its,
        #include "packet-its-ettarr.c"
    };

    proto_its = proto_register_protocol("Intelligent Transport Systems", "ITS", "its");

    proto_register_field_array(proto_its, hf_its, array_length(hf_its));

    proto_register_subtree_array(ett, array_length(ett));

    register_dissector("its", dissect_its_PDU, proto_its);

    // Register subdissector table
    its_version_subdissector_table = register_dissector_table("its.version", "ITS version", proto_its, FT_UINT8, BASE_DEC);
    its_msgid_subdissector_table = register_dissector_table("its.msg_id", "ITS message id", proto_its, FT_UINT32, BASE_DEC);
    regionid_subdissector_table = register_dissector_table("dsrc.regionid", "DSRC RegionId", proto_its, FT_UINT32, BASE_DEC);

    proto_its_denm = proto_register_protocol_in_name_only("ITS message - DENM", "DENM", "its.message.denm", proto_its, FT_BYTES);
    proto_its_cam = proto_register_protocol_in_name_only("ITS message - CAM", "CAM", "its.message.cam", proto_its, FT_BYTES);
    proto_its_spatem = proto_register_protocol_in_name_only("ITS message - SPATEM", "SPATEM", "its.message.spatem", proto_its, FT_BYTES);
    proto_its_mapem = proto_register_protocol_in_name_only("ITS message - MAPEM", "MAPEM", "its.message.mapem", proto_its, FT_BYTES);
    proto_its_ivim = proto_register_protocol_in_name_only("ITS message - IVIM", "IVIM", "its.message.ivim", proto_its, FT_BYTES);
    proto_its_evrsr = proto_register_protocol_in_name_only("ITS message - EVRSR", "EVRSR", "its.message.evrsr", proto_its, FT_BYTES);
    proto_its_srem = proto_register_protocol_in_name_only("ITS message - SREM", "SREM", "its.message.srem", proto_its, FT_BYTES);
    proto_its_ssem = proto_register_protocol_in_name_only("ITS message - SSEM", "SSEM", "its.message.ssem", proto_its, FT_BYTES);
    proto_its_evcsn = proto_register_protocol_in_name_only("ITS message - EVCSN", "EVCSN", "its.message.evcsn", proto_its, FT_BYTES);
    proto_its_tistpg = proto_register_protocol_in_name_only("ITS message - TISTPG", "TISTPG", "its.message.tistpg", proto_its, FT_BYTES);

    proto_addgrpc = proto_register_protocol_in_name_only("DSRC Addition Grp C (EU)", "ADDGRPC", "dsrc.addgrpc", proto_its, FT_BYTES);

    // Decode as
    static build_valid_func its_da_build_value[1] = {its_msgid_value};
    static decode_as_value_t its_da_values = {its_msgid_prompt, 1, its_da_build_value};
    static decode_as_t its_da = {"its", "ITS msg id", "its.msg_id", 1, 0, &its_da_values, NULL, NULL,
                                    decode_as_default_populate_list, decode_as_default_reset, decode_as_default_change, NULL};

    register_decode_as(&its_da);
}

#define BTP_SUBDISS_SZ 2
#define BTP_PORTS_SZ   10
void proto_reg_handoff_its(void)
{
    const char *subdissector[BTP_SUBDISS_SZ] = { "btpa.port", "btpb.port" };
    const guint16 ports[BTP_PORTS_SZ] = { ITS_WKP_DEN, ITS_WKP_CA, ITS_WKP_EVCSN, ITS_WKP_CHARGING, ITS_WKP_IVI, ITS_WKP_TPG, ITS_WKP_TLC_SSEM, ITS_WKP_TLC_SREM, ITS_WKP_RLT, ITS_WKP_TLM };
    int sdIdx, pIdx;
    dissector_handle_t its_handle_;

    // Register well known ports to btp subdissector table (BTP A and B)
    its_handle_ = create_dissector_handle(dissect_its_PDU, proto_its);
    for (sdIdx=0; sdIdx < BTP_SUBDISS_SZ; sdIdx++) {
        for (pIdx=0; pIdx < BTP_PORTS_SZ; pIdx++) {
            dissector_add_uint(subdissector[sdIdx], ports[pIdx], its_handle_);
        }
    }

    dissector_add_uint("geonw.sec.v1.msg_type", ITS_DENM, its_handle_);
    dissector_add_uint("geonw.sec.v1.msg_type", ITS_CAM, its_handle_);
    dissector_add_uint("geonw.sec.v2.app_id", AID_DEN, its_handle_);
    dissector_add_uint("geonw.sec.v2.app_id", AID_CA, its_handle_);
    dissector_add_uint("geonw.sec.v2.app_id", AID_TLM, its_handle_);
    dissector_add_uint("geonw.sec.v2.app_id", AID_RLT, its_handle_);
    dissector_add_uint("geonw.sec.v2.app_id", AID_IVI, its_handle_);
    dissector_add_uint("geonw.sec.v2.app_id", AID_TLC, its_handle_);

    dissector_add_uint("its.msg_id", ITS_DENM,              create_dissector_handle( dissect_denm_DecentralizedEnvironmentalNotificationMessage_PDU, proto_its_denm ));
    dissector_add_uint("its.msg_id", ITS_CAM,               create_dissector_handle( dissect_cam_CoopAwareness_PDU, proto_its_cam ));
    dissector_add_uint("its.msg_id", ITS_SPATEM,            create_dissector_handle( dissect_dsrc_SPAT_PDU, proto_its_spatem ));
    dissector_add_uint("its.msg_id", ITS_MAPEM,             create_dissector_handle( dissect_dsrc_MapData_PDU, proto_its_mapem ));
    dissector_add_uint("its.msg_id", ITS_IVIM,              create_dissector_handle( dissect_ivi_IviStructure_PDU, proto_its_ivim ));
    dissector_add_uint("its.msg_id", ITS_EV_RSR,            create_dissector_handle( dissect_evrsr_EV_RSR_MessageBody_PDU, proto_its_evrsr ));
    dissector_add_uint("its.msg_id", ITS_SREM,              create_dissector_handle( dissect_dsrc_SignalRequestMessage_PDU, proto_its_srem ));
    dissector_add_uint("its.msg_id", ITS_SSEM,              create_dissector_handle( dissect_dsrc_SignalStatusMessage_PDU, proto_its_ssem ));
    dissector_add_uint("its.msg_id", ITS_EVCSN,             create_dissector_handle( dissect_evcsn_EVChargingSpotNotificationPOIMessage_PDU, proto_its_evcsn ));
    dissector_add_uint("its.msg_id", ITS_TISTPGTRANSACTION, create_dissector_handle( dissect_tistpg_TisTpgTransaction_PDU, proto_its_tistpg ));

    /* Missing definitions: ITS_POI, ITS_SAEM, ITS_RTCMEM */

    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_ConnectionManeuverAssist, create_dissector_handle(dissect_AddGrpC_ConnectionManeuverAssist_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_GenericLane, create_dissector_handle(dissect_AddGrpC_ConnectionTrajectory_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_NodeAttributeSetXY, create_dissector_handle(dissect_AddGrpC_Control_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_IntersectionState, create_dissector_handle(dissect_AddGrpC_IntersectionState_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_MapData, create_dissector_handle(dissect_AddGrpC_MapData_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_Position3D, create_dissector_handle(dissect_AddGrpC_Position3D_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_RestrictionUserType, create_dissector_handle(dissect_AddGrpC_RestrictionUserType_addGrpC_PDU, proto_addgrpc ));
    dissector_add_uint("dsrc.regionid", (addGrpC<<16)+Reg_SignalStatusPackage, create_dissector_handle(dissect_AddGrpC_SignalStatusPackage_addGrpC_PDU, proto_addgrpc ));

    its_tap = register_tap("its");
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
