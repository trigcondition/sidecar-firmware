// plugins/mdkit/families.h - the voices of each mdkit family, in TYPE order (P9's names): one of MDK_FAM_PI,
// MDK_FAM_EFM, MDK_FAM_TRX (the Makefile's FAM). X(voice) per voice: mdkit's mdkit_<voice> state and
// mdkit_voice_<voice> table. MDK_FAM_MD: the three in one machine, in ENGINE order (FM, PI, TRX: P9's names), each
// ENGINE's voices in its TYPE order (P10's names); MDK_ENGINES(X) is X(its TYPE count) per ENGINE.
// MIT, Copyright 2026 trigcondition and contributors; the voices are mdkit's (CC0).
#pragma once
#define MDK_EFM(X) X(efm_bd) X(efm_sd) X(efm_xt) X(efm_cp) X(efm_rs) X(efm_cb) X(efm_hh) X(efm_cy)
#define MDK_PI(X) X(pi_bd) X(pi_sd) X(pi_mt) X(pi_ml) X(pi_ma) X(pi_rs) X(pi_rc) X(pi_cc) X(pi_hh)
#define MDK_TRX(X) X(trx_bd) X(trx_sd) X(trx_xt) X(trx_cp) X(trx_rs) X(trx_cb) X(trx_ch) X(trx_oh) X(trx_cy) \
  X(trx_ma) X(trx_cl) X(trx_xc) X(trx_b2)
#if defined(MDK_FAM_MD) || defined(MDK_FAM_EFM)
#include "mdkit_efm_bd.h"
#include "mdkit_efm_sd.h"
#include "mdkit_efm_xt.h"
#include "mdkit_efm_cp.h"
#include "mdkit_efm_rs.h"
#include "mdkit_efm_cb.h"
#include "mdkit_efm_hh.h"
#include "mdkit_efm_cy.h"
#endif
#if defined(MDK_FAM_MD) || defined(MDK_FAM_PI)
#include "mdkit_pi_bd.h"
#include "mdkit_pi_sd.h"
#include "mdkit_pi_mt.h"
#include "mdkit_pi_ml.h"
#include "mdkit_pi_ma.h"
#include "mdkit_pi_rs.h"
#include "mdkit_pi_rc.h"
#include "mdkit_pi_cc.h"
#include "mdkit_pi_hh.h"
#endif
#if defined(MDK_FAM_MD) || defined(MDK_FAM_TRX)
#include "mdkit_trx_bd.h"
#include "mdkit_trx_sd.h"
#include "mdkit_trx_xt.h"
#include "mdkit_trx_cp.h"
#include "mdkit_trx_rs.h"
#include "mdkit_trx_cb.h"
#include "mdkit_trx_ch.h"
#include "mdkit_trx_oh.h"
#include "mdkit_trx_cy.h"
#include "mdkit_trx_ma.h"
#include "mdkit_trx_cl.h"
#include "mdkit_trx_xc.h"
#include "mdkit_trx_b2.h"
#endif
#if defined(MDK_FAM_MD)
#define MDK_VOICES(X) MDK_EFM(X) MDK_PI(X) MDK_TRX(X)
#define MDK_ENGINES(X) X(8) X(9) X(13)
#elif defined(MDK_FAM_PI)
#define MDK_VOICES(X) MDK_PI(X)
#elif defined(MDK_FAM_EFM)
#define MDK_VOICES(X) MDK_EFM(X)
#elif defined(MDK_FAM_TRX)
#define MDK_VOICES(X) MDK_TRX(X)
#else
#error "MDK_FAM_MD, MDK_FAM_PI, MDK_FAM_EFM or MDK_FAM_TRX"
#endif
