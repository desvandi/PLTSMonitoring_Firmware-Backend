#!/usr/bin/env node
/**
 * harness-setup-fix.js — shared helper snippet for the GAS JS test harnesses.
 * =============================================================================
 * [PARITY-3 FIX 2026-09-06] setupMasterTemplate() is P1-8 fail-closed: with
 * the default EMPTY AUTH_TOKEN it refuses deployment at the END of setup —
 * but all sheets and DEFAULT_CONFIG rows are already written by then. The
 * three Node harnesses (test_gas_contract.js, test_emergency_gas.js,
 * test_wave1_integration.js) predate that guard: the WAVE-1 template used
 * to ship a default token, so their direct setupMasterTemplate() calls now
 * die with "AUTH_TOKEN not configured" on every run (dead harnesses — CI
 * only runs the Python suite, so nobody noticed).
 *
 * The fix, applied by patch_harness_setup_(): call setup ONCE, catch the
 * expected first-run refusal, seed a strong test AUTH_TOKEN into the Config
 * sheet, and invalidate the config cache. A second setup call is NOT made —
 * setupMasterTemplate clears and rewrites the Config sheet from
 * DEFAULT_CONFIG, which would erase the seed and throw again.
 *
 * This snippet is applied to each harness file by scripts/patch (kept here
 * as the canonical reference + for source-structure tests to assert on).
 * =============================================================================
 */
'use strict';

/**
 * Patch a harness's env so its setupMasterTemplate() call survives the P1-8
 * fail-closed guard. Usage inside a harness:
 *   const env = createGasContext();
 *   patchHarnessSetup_(env);          // instead of env.sandbox.setupMasterTemplate()
 */
function patchHarnessSetup_(env, token) {
  const t = token || 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE';
  try {
    env.sandbox.setupMasterTemplate();
  } catch (e) {
    // Expected P1-8 refusal on a fresh sheet: AUTH_TOKEN defaults to ''.
    // Sheets + DEFAULT_CONFIG are already written when it throws. Anything
    // OTHER than the AUTH_TOKEN refusal is a genuine setup failure —
    // rethrow so the harness fails loudly rather than silently.
    if (!/AUTH_TOKEN not configured/.test(String(e && e.message))) {
      throw e;
    }
  }
  const cfg = env.ss.sheets['Config'];
  if (!cfg) {
    throw new Error('patchHarnessSetup_: Config sheet missing after setup');
  }
  const rows = cfg.getDataRange().getValues();
  let seeded = false;
  for (let i = 0; i < rows.length; i++) {
    if (String(rows[i][0]) === 'AUTH_TOKEN') {
      cfg.getRange(i + 1, 2).setValue(t);
      seeded = true;
    }
  }
  if (!seeded) cfg.appendRow(['AUTH_TOKEN', t]);
  env.sandbox.invalidatePltsCache();
}

module.exports = { patchHarnessSetup_ };
