/*
 * WebPushCore - Implementasi Web Push (RFC 8030/8291/8292/8188) murni JS.
 * ====================================================================
 * Berjalan di Google Apps Script (V8) maupun Node.js (untuk pengujian).
 *
 * Isi:
 *  - SHA-256, HMAC-SHA256, HKDF (RFC 5869)
 *  - Aritmetika P-256: Montgomery 16-bit limb, titik Jacobian,
 *    ECDSA tanda-tangan (RFC 6979, nonce deterministik),
 *    ECDH (validasi titik on-curve).
 *  - AES-128-GCM (dipakai untuk enkripsi payload aes128gcm).
 *  - VAPID: JWT ES256 + header Authorization (RFC 8292).
 *  - Enkripsi payload Web Push (RFC 8291 + RFC 8188).
 *
 * Catatan kompatibilitas GAS:
 *  - Tanpa BigInt, tanpa WebCrypto, tanpa Buffer.
 *  - Semua nama global dibungkus satu objek `WebPushCore` (var) agar
 *    aman lintas file .gs dalam satu proyek Apps Script.
 *  - Semua tabel (K SHA-256, S-box AES) dihitung programatik saat
 *    inisialisasi -> bebas risiko salah transkripsi.
 */
'use strict';

var WebPushCore = (function () {

  /* ================================================================
   * 0. UTILITAS BYTE / STRING
   * ================================================================ */

  function utf8Encode(str) {
    // encodeURIComponent -> %XX per byte UTF-8, lalu petakan kembali.
    return Array.prototype.map.call(unescape(encodeURIComponent(str)), function (c) {
      return c.charCodeAt(0) & 0xff;
    });
  }

  function utf8Decode(bytes) {
    var bin = '';
    for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i] & 0xff);
    return decodeURIComponent(escape(bin));
  }

  function bytesToHex(bytes) {
    var s = '';
    for (var i = 0; i < bytes.length; i++) s += (bytes[i] + 0x100).toString(16).slice(1);
    return s;
  }

  function concatBytes() {
    var total = 0, i, a;
    for (i = 0; i < arguments.length; i++) total += arguments[i].length;
    var out = new Array(total);
    var o = 0;
    for (i = 0; i < arguments.length; i++) {
      a = arguments[i];
      for (var j = 0; j < a.length; j++) out[o++] = a[j] & 0xff;
    }
    return out;
  }

  /* ---- Base64url (RFC 4648 §5, tanpa padding) ---- */

  var B64_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

  function b64urlEncode(bytes) {
    var out = '', i;
    for (i = 0; i + 2 < bytes.length; i += 3) {
      var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
      out += B64_ALPHABET[(n >>> 18) & 63] + B64_ALPHABET[(n >>> 12) & 63] +
             B64_ALPHABET[(n >>> 6) & 63] + B64_ALPHABET[n & 63];
    }
    var rem = bytes.length - i;
    if (rem === 1) {
      var n1 = bytes[i] << 16;
      out += B64_ALPHABET[(n1 >>> 18) & 63] + B64_ALPHABET[(n1 >>> 12) & 63];
    } else if (rem === 2) {
      var n2 = (bytes[i] << 16) | (bytes[i + 1] << 8);
      out += B64_ALPHABET[(n2 >>> 18) & 63] + B64_ALPHABET[(n2 >>> 12) & 63] +
             B64_ALPHABET[(n2 >>> 6) & 63];
    }
    return out;
  }

  function b64urlDecode(str) {
    str = String(str).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    var out = [], i, buf = 0, bits = 0;
    for (i = 0; i < str.length; i++) {
      var v = B64_ALPHABET.indexOf(str.charAt(i));
      if (v < 0) throw new Error('b64url: karakter tidak valid');
      buf = (buf << 6) | v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push((buf >>> bits) & 0xff);
      }
    }
    return out;
  }

  /* ---- Sumber acak (urutan prioritas) ---- */

  function randomBytes(n) {
    var out = new Array(n);
    // 1) WebCrypto (browser SW/Node>=19 global crypto)
    try {
      if (typeof crypto !== 'undefined' && crypto.getRandomValues) {
        var u8 = new Uint8Array(n);
        crypto.getRandomValues(u8);
        for (var i = 0; i < n; i++) out[i] = u8[i];
        return out;
      }
    } catch (e) { /* lanjut */ }
    // 2) GAS Utilities.getUuid (122-bit entropi versi 4)
    try {
      if (typeof Utilities !== 'undefined' && Utilities.getUuid) {
        var filled = 0;
        while (filled < n) {
          var hex = Utilities.getUuid().replace(/-/g, '');
          for (var j = 0; j < hex.length && filled < n; j += 2) {
            out[filled++] = parseInt(hex.slice(j, j + 2), 16);
          }
        }
        return out;
      }
    } catch (e) { /* lanjut */ }
    // 3) Math.random (fallback terakhir - hanya untuk salt)
    for (var k = 0; k < n; k++) out[k] = Math.floor(Math.random() * 256);
    return out;
  }

  /* ================================================================
   * 1. SHA-256 + HMAC + HKDF
   * ================================================================ */

  // K & H awal dihitung dari akar pangkat 2/3 bilangan prima
  // (fraksi * 2^32) -> presisi double (>=49 bit signifikan) memadai.
  var SHA_K = (function () {
    var primes = [], c = 2;
    while (primes.length < 64) {
      var isP = true;
      for (var d = 2; d * d <= c; d++) if (c % d === 0) { isP = false; break; }
      if (isP) primes.push(c);
      c++;
    }
    return primes.map(function (p) {
      var f = Math.cbrt(p);
      f -= Math.floor(f);
      return Math.floor(f * 4294967296);
    });
  })();

  var SHA_H = (function () {
    var primes = [], c = 2;
    while (primes.length < 8) {
      var isP = true;
      for (var d = 2; d * d <= c; d++) if (c % d === 0) { isP = false; break; }
      if (isP) primes.push(c);
      c++;
    }
    return primes.map(function (p) {
      var f = Math.sqrt(p);
      f -= Math.floor(f);
      return Math.floor(f * 4294967296);
    });
  })();

  function rotr32(x, n) { return (x >>> n) | (x << (32 - n)); }

  function sha256(bytes) {
    var H = SHA_H.slice();
    var lenBits = bytes.length * 8;
    // Padding
    var msg = bytes.slice();
    msg.push(0x80);
    while (msg.length % 64 !== 56) msg.push(0);
    for (var i = 7; i >= 0; i--) msg.push((lenBits / Math.pow(2, i * 8)) & 0xff);

    var w = new Array(64);
    for (var block = 0; block < msg.length; block += 64) {
      for (var t = 0; t < 16; t++) {
        w[t] = (msg[block + t * 4] << 24) | (msg[block + t * 4 + 1] << 16) |
               (msg[block + t * 4 + 2] << 8) | msg[block + t * 4 + 3];
      }
      for (var t2 = 16; t2 < 64; t2++) {
        var s0 = rotr32(w[t2 - 15], 7) ^ rotr32(w[t2 - 15], 18) ^ (w[t2 - 15] >>> 3);
        var s1 = rotr32(w[t2 - 2], 17) ^ rotr32(w[t2 - 2], 19) ^ (w[t2 - 2] >>> 10);
        w[t2] = (w[t2 - 16] + s0 + w[t2 - 7] + s1) | 0;
      }
      var a = H[0], b = H[1], c = H[2], d = H[3];
      var e = H[4], f = H[5], g = H[6], h = H[7];
      for (var t3 = 0; t3 < 64; t3++) {
        var S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        var ch = (e & f) ^ (~e & g);
        var temp1 = (h + S1 + ch + SHA_K[t3] + w[t3]) | 0;
        var S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        var maj = (a & b) ^ (a & c) ^ (b & c);
        var temp2 = (S0 + maj) | 0;
        h = g; g = f; f = e;
        e = (d + temp1) | 0;
        d = c; c = b; b = a;
        a = (temp1 + temp2) | 0;
      }
      H[0] = (H[0] + a) | 0; H[1] = (H[1] + b) | 0;
      H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
      H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0;
      H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
    }
    var out = [];
    for (var j = 0; j < 8; j++) {
      out.push((H[j] >>> 24) & 0xff, (H[j] >>> 16) & 0xff, (H[j] >>> 8) & 0xff, H[j] & 0xff);
    }
    return out;
  }

  function hmacSha256(key, message) {
    var k = key.slice();
    if (k.length > 64) k = sha256(k);
    while (k.length < 64) k.push(0);
    var oKey = k.map(function (b) { return b ^ 0x5c; });
    var iKey = k.map(function (b) { return b ^ 0x36; });
    return sha256(concatBytes(oKey, sha256(concatBytes(iKey, message))));
  }

  function hkdf(salt, ikm, info, length) {
    var prk = hmacSha256(salt, ikm);
    var t = [], okm = [], i = 1;
    while (okm.length < length) {
      t = hmacSha256(prk, concatBytes(t, info, [i]));
      okm = okm.concat(t);
      i++;
      if (i > 255) throw new Error('hkdf: panjang melebihi batas');
    }
    return okm.slice(0, length);
  }

  /* ================================================================
   * 2. ARITMETIKA MODULAR P-256 (Montgomery, 16 limb x 16 bit)
   * ================================================================ */

  /* Parameter domain P-256 (SECG SEC 2) - limb little-endian */
  var P = [0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0x0000, 0x0000,
           0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0xffff, 0xffff];
  var N = [0x2551, 0xfc63, 0xcac2, 0xf3b9, 0x9e84, 0xa717, 0xfaad, 0xbce6,
           0xffff, 0xffff, 0xffff, 0xffff, 0x0000, 0x0000, 0xffff, 0xffff];
  var B = [0x604b, 0x27d2, 0x3c3e, 0x3bce, 0xb0f6, 0xcc53, 0x06b0, 0x651d,
           0x86bc, 0x7698, 0xbd55, 0xb3eb, 0x93e7, 0xaa3a, 0x35d8, 0x5ac6];
  var GX = [0xc296, 0xd898, 0x3945, 0xf4a1, 0x33a0, 0x2deb, 0x7d81, 0x7703,
            0x40f2, 0x63a4, 0xe6e5, 0xf8bc, 0x4247, 0xe12c, 0xd1f2, 0x6b17];
  var GY = [0x51f5, 0x37bf, 0x4068, 0xcbb6, 0x5ece, 0x6b31, 0x3357, 0x2bce,
            0x9e16, 0x7c0f, 0xeb4a, 0x8ee7, 0x7f9b, 0xfe1a, 0x42e2, 0x4fe3];

  /* Konstanta Montgomery (diverifikasi via BigInt saat pembuatan):
   * np0 = -m^-1 mod 2^16, r2 = R^2 mod m, R = 2^256 */
  var MONT_P = {
    np0: 0x0001,
    r2: [0x0003, 0x0000, 0x0000, 0x0000, 0xffff, 0xffff, 0xfffb, 0xffff,
         0xfffe, 0xffff, 0xffff, 0xffff, 0xfffd, 0xffff, 0x0004, 0x0000]
  };
  var MONT_N = {
    np0: 0xbc4f,
    r2: [0xeea2, 0xbe79, 0x4c95, 0x8324, 0x6fa6, 0x49bd, 0x799c, 0x4699,
         0xec59, 0x2b6b, 0xb239, 0x2845, 0x5620, 0xf3d9, 0x2d94, 0x66e1]
  };
  var P_MINUS_2 = [0xfffd, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0x0000, 0x0000,
                   0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0000, 0xffff, 0xffff];
  var N_MINUS_2 = [0x254f, 0xfc63, 0xcac2, 0xf3b9, 0x9e84, 0xa717, 0xfaad, 0xbce6,
                   0xffff, 0xffff, 0xffff, 0xffff, 0x0000, 0x0000, 0xffff, 0xffff];

  function zeroLimbs() { return [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]; }
  function oneLimbs() { var r = zeroLimbs(); r[0] = 1; return r; }

  function limbsCmp(a, b) {
    for (var i = 15; i >= 0; i--) {
      if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
  }

  function limbsIsZero(a) {
    for (var i = 0; i < 16; i++) if (a[i] !== 0) return false;
    return true;
  }

  /* Montgomery multiplication: hasil = a*b*R^-1 mod m.
   * a, b harus < m (invariant dijaga semua fungsi publik). */
  function montMul(a, b, mont) {
    var m = mont === MONT_P ? P : N;
    var T = new Array(36);
    for (var z = 0; z < 36; z++) T[z] = 0;

    // T = a * b (32+ limb)
    for (var i = 0; i < 16; i++) {
      var ai = a[i];
      if (ai === 0) continue;
      var carry = 0;
      for (var j = 0; j < 16; j++) {
        var uv = T[i + j] + ai * b[j] + carry;
        T[i + j] = uv & 0xffff;
        carry = uv >>> 16;
      }
      var k = i + 16;
      while (carry !== 0) {
        var uv2 = T[k] + carry;
        T[k] = uv2 & 0xffff;
        carry = uv2 >>> 16;
        k++;
      }
    }

    // Reduksi Montgomery: T += mu*m pada offset i, mu = T[i]*np0 mod 2^16
    for (var i2 = 0; i2 < 16; i2++) {
      var mu = (T[i2] * mont.np0) & 0xffff;
      var carry2 = 0;
      for (var j2 = 0; j2 < 16; j2++) {
        var uv3 = T[i2 + j2] + mu * m[j2] + carry2;
        T[i2 + j2] = uv3 & 0xffff;
        carry2 = uv3 >>> 16;
      }
      var k2 = i2 + 16;
      while (carry2 !== 0) {
        var uv4 = T[k2] + carry2;
        T[k2] = uv4 & 0xffff;
        carry2 = uv4 >>> 16;
        k2++;
      }
    }

    // Hasil ada di T[16..32]; kurangi m sampai < m (maks 2x).
    var res = new Array(17);
    for (var r = 0; r < 16; r++) res[r] = T[16 + r];
    res[16] = T[32];
    for (var sub = 0; sub < 2; sub++) {
      var needSub = res[16] !== 0;
      if (!needSub) {
        var m16 = m.slice(); m16.push(0);
        if (limbsCmp17(res, m16) >= 0) needSub = true;
      }
      if (!needSub) break;
      // res -= m
      var borrow = 0;
      for (var s = 0; s < 16; s++) {
        var d = res[s] - m[s] - borrow;
        if (d < 0) { d += 0x10000; borrow = 1; } else { borrow = 0; }
        res[s] = d;
      }
      res[16] -= borrow;
    }
    if (res[16] !== 0) throw new Error('montMul: hasil reduksi >= 2m');
    return res.slice(0, 16);
  }

  function limbsCmp17(a, b) {
    if (a[16] !== b[16]) return a[16] < b[16] ? -1 : 1;
    return limbsCmp(a, b);
  }

  /* Perkalian modular biasa: hasil = a*b mod m */
  function modMul(a, b, mont) {
    return montMul(montMul(a, mont.r2, mont), b, mont);
  }

  function modAdd(a, b, mont) {
    var m = mont === MONT_P ? P : N;
    var res = new Array(17);
    var carry = 0;
    for (var i = 0; i < 16; i++) {
      var s = a[i] + b[i] + carry;
      res[i] = s & 0xffff;
      carry = s >>> 16;
    }
    res[16] = carry;
    if (res[16] !== 0 || limbsCmp(res, m) >= 0) {
      var borrow = 0;
      for (var j = 0; j < 16; j++) {
        var d = res[j] - m[j] - borrow;
        if (d < 0) { d += 0x10000; borrow = 1; } else { borrow = 0; }
        res[j] = d;
      }
      res[16] -= borrow;
    }
    return res.slice(0, 16);
  }

  function modSub(a, b, mont) {
    var m = mont === MONT_P ? P : N;
    if (limbsCmp(a, b) >= 0) {
      var out = new Array(16);
      var borrow = 0;
      for (var i = 0; i < 16; i++) {
        var d = a[i] - b[i] - borrow;
        if (d < 0) { d += 0x10000; borrow = 1; } else { borrow = 0; }
        out[i] = d;
      }
      return out;
    }
    // Kasus a < b: hasil = a + m - b, dijamin 0 < hasil < m.
    // Hitung dengan 17 limb agar tidak kehilangan carry dari a + m.
    var res = new Array(17);
    var carry = 0;
    for (var j = 0; j < 16; j++) {
      var s = a[j] + m[j] + carry;
      res[j] = s & 0xffff;
      carry = s >>> 16;
    }
    res[16] = carry;
    var borrow2 = 0;
    for (var k = 0; k < 16; k++) {
      var d2 = res[k] - b[k] - borrow2;
      if (d2 < 0) { d2 += 0x10000; borrow2 = 1; } else { borrow2 = 0; }
      res[k] = d2;
    }
    res[16] -= borrow2;
    if (res[16] !== 0) throw new Error('modSub: underflow');
    return res.slice(0, 16);
  }

  /* Eksponensiasi modular (square & multiply, MSB->LSB) */
  function modExp(a, e, mont) {
    var result = oneLimbs();
    for (var i = 255; i >= 0; i--) {
      result = modMul(result, result, mont);
      var limb = e[i >> 4];
      if ((limb >>> (i & 15)) & 1) result = modMul(result, a, mont);
    }
    return result;
  }

  /* Invers modular via Fermat (p dan n keduanya prima) */
  function modInv(a, mont) {
    if (limbsIsZero(a)) throw new Error('modInv: invers dari nol');
    var mMinus2 = mont === MONT_P ? P_MINUS_2 : N_MINUS_2;
    return modExp(a, mMinus2, mont);
  }

  /* ---- Konversi bytes <-> limb ---- */

  function bytes32ToLimbs(bytes) {
    if (bytes.length !== 32) throw new Error('bytes32ToLimbs: butuh 32 byte');
    var out = zeroLimbs();
    for (var i = 0; i < 32; i++) {
      var limbIndex = (31 - i) >> 1;      // byte 31 -> limb 0 (LE)
      var shift = ((31 - i) & 1) === 0 ? 0 : 8;
      out[limbIndex] |= (bytes[i] & 0xff) << shift;
    }
    return out;
  }

  function limbsToBytes32(limbs) {
    var out = new Array(32);
    for (var i = 0; i < 32; i++) {
      var limbIndex = (31 - i) >> 1;
      var shift = ((31 - i) & 1) === 0 ? 0 : 8;
      out[i] = (limbs[limbIndex] >>> shift) & 0xff;
    }
    return out;
  }

  /* Bytes 32-byte BE -> limb tereduksi mod m */
  function bytesToModLimbs(bytes, mont) {
    var v = bytes32ToLimbs(bytes);
    var m = mont === MONT_P ? P : N;
    while (limbsCmp(v, m) >= 0) {
      v = modSub(v, m, mont);
    }
    return v;
  }

  function limbsToHex(limbs) {
    return bytesToHex(limbsToBytes32(limbs));
  }

  /* ================================================================
   * 3. TITIK ELIPTIK P-256 (koordinat Jacobian)
   * ================================================================ */

  var INFINITY = null; // titik tak hingga direpresentasikan Z=0

  function isInfinity(pt) { return pt === null || limbsIsZero(pt.z); }

  function affineToPoint(x, y) { return { x: x.slice(), y: y.slice(), z: oneLimbs() }; }

  function pointDouble(pt) {
    if (isInfinity(pt)) return pt;
    var X = pt.x, Y = pt.y, Z = pt.z;
    if (limbsIsZero(Y)) return INFINITY;

    // a = -3:  M = 3*(X - Z^2)*(X + Z^2)
    var zz = modMul(Z, Z, MONT_P);
    var xMZ = modSub(X, zz, MONT_P);
    var xPZ = modAdd(X, zz, MONT_P);
    var m = modMul(xMZ, xPZ, MONT_P);
    m = modAdd(m, m, MONT_P);           // 2*(X-Z^2)(X+Z^2)
    m = modAdd(m, modMul(xMZ, xPZ, MONT_P), MONT_P); // 3*(...)

    var yy = modMul(Y, Y, MONT_P);
    var s = modMul(X, yy, MONT_P);      // X*Y^2
    s = modAdd(s, s, MONT_P);
    s = modAdd(s, s, MONT_P);           // 4*X*Y^2

    var x3 = modMul(m, m, MONT_P);
    x3 = modSub(x3, modAdd(s, s, MONT_P), MONT_P);   // M^2 - 2S

    // Y3 = M*(S - X3) - 8*Y^4,  Y^4 = (Y^2)^2
    var y4 = modMul(yy, yy, MONT_P);
    var t8 = modAdd(y4, y4, MONT_P);
    t8 = modAdd(t8, t8, MONT_P);
    t8 = modAdd(t8, t8, MONT_P);        // 8*Y^4
    var y3 = modMul(m, modSub(s, x3, MONT_P), MONT_P);
    y3 = modSub(y3, t8, MONT_P);

    // Z3 = 2*Y*Z
    var z3 = modMul(Y, Z, MONT_P);
    z3 = modAdd(z3, z3, MONT_P);

    return { x: x3, y: y3, z: z3 };
  }

  /* Penjumlahan Jacobian + affine (mixed addition) */
  function pointAddMixed(pt, qAffine) {
    if (isInfinity(pt)) {
      return { x: qAffine.x.slice(), y: qAffine.y.slice(), z: oneLimbs() };
    }
    var X1 = pt.x, Y1 = pt.y, Z1 = pt.z;
    var X2 = qAffine.x, Y2 = qAffine.y;

    var zz = modMul(Z1, Z1, MONT_P);
    var zzz = modMul(zz, Z1, MONT_P);

    var U2 = modMul(X2, zz, MONT_P);
    var S2 = modMul(Y2, zzz, MONT_P);

    var h = modSub(U2, X1, MONT_P);
    var r = modSub(S2, Y1, MONT_P);

    if (limbsIsZero(h)) {
      if (limbsIsZero(r)) return pointDouble(pt);
      return INFINITY;
    }

    var hh = modMul(h, h, MONT_P);
    var hhh = modMul(hh, h, MONT_P);
    var v = modMul(X1, hh, MONT_P);

    // X3 = r^2 - hhh - 2V
    var x3 = modMul(r, r, MONT_P);
    x3 = modSub(x3, hhh, MONT_P);
    x3 = modSub(x3, modAdd(v, v, MONT_P), MONT_P);

    // Y3 = r*(V - X3) - Y1*hhh
    var y3 = modMul(r, modSub(v, x3, MONT_P), MONT_P);
    y3 = modSub(y3, modMul(Y1, hhh, MONT_P), MONT_P);

    // Z3 = Z1*H
    var z3 = modMul(Z1, h, MONT_P);

    return { x: x3, y: y3, z: z3 };
  }

  function pointToAffine(pt) {
    if (isInfinity(pt)) throw new Error('pointToAffine: titik tak hingga');
    var zInv = modInv(pt.z, MONT_P);
    var zInv2 = modMul(zInv, zInv, MONT_P);
    var zInv3 = modMul(zInv2, zInv, MONT_P);
    return {
      x: modMul(pt.x, zInv2, MONT_P),
      y: modMul(pt.y, zInv3, MONT_P)
    };
  }

  /* Perkalian skalar: k (limbs) * titik affine -> affine */
  function scalarMult(k, affinePoint) {
    var result = INFINITY;
    for (var i = 255; i >= 0; i--) {
      result = pointDouble(result);
      var limb = k[i >> 4];
      if ((limb >>> (i & 15)) & 1) {
        result = pointAddMixed(result, affinePoint);
      }
    }
    if (isInfinity(result)) throw new Error('scalarMult: hasil titik tak hingga');
    return pointToAffine(result);
  }

  /* Validasi titik pada kurva: y^2 = x^3 - 3x + b (mod p) */
  function isOnCurve(affine) {
    var lhs = modMul(affine.y, affine.y, MONT_P);
    var x3 = modMul(affine.x, modMul(affine.x, affine.x, MONT_P), MONT_P);
    var threeX = modMul(affine.x, [3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], MONT_P);
    var rhs = modAdd(modSub(x3, threeX, MONT_P), B, MONT_P);
    return limbsCmp(lhs, rhs) === 0;
  }

  /* Kunci publik (uncompressed 65 byte: 0x04||X||Y) -> affine */
  function parsePublicKey(bytes65) {
    if (!bytes65 || bytes65.length !== 65 || bytes65[0] !== 0x04) {
      throw new Error('Kunci publik harus 65 byte tidak-terkompresi (0x04||X||Y)');
    }
    var x = bytes32ToLimbs(bytes65.slice(1, 33));
    var y = bytes32ToLimbs(bytes65.slice(33, 65));
    var pt = { x: x, y: y };
    if (!isOnCurve(pt)) throw new Error('Kunci publik tidak berada pada kurva P-256');
    if (limbsCmp(x, P) >= 0 || limbsCmp(y, P) >= 0) throw new Error('Koordinat kunci publik >= p');
    return pt;
  }

  /* Turunkan kunci publik dari kunci privat (bytes32).
   * Skalar pendek (< 32 byte, mis. akibat byte nol di depan dibuang
   * oleh ekspor DER/Node crypto) diterima dengan pad nol di kiri
   * (temuan X-8 audit silang: ~0,4% pasangan kunci VAPID mengalaminya
   * dan tanpa pad ini seluruh pengiriman push gagal). */
  function padScalar32_(bytes) {
    if (!bytes || bytes.length === 0) throw new Error('skalar kosong');
    if (bytes.length === 32) return bytes;
    if (bytes.length > 32) throw new Error('skalar > 32 byte');
    var out = new Array(32).fill(0);
    for (var i = 0; i < bytes.length; i++) out[32 - bytes.length + i] = bytes[i];
    return out;
  }

  function publicKeyFromPrivate(privBytes32) {
    var d = bytesToModLimbs(padScalar32_(privBytes32), MONT_N);
    if (limbsIsZero(d)) throw new Error('Kunci privat nol');
    var pub = scalarMult(d, { x: GX, y: GY });
    return concatBytes([0x04], limbsToBytes32(pub.x), limbsToBytes32(pub.y));
  }

  /* ================================================================
   * 4. ECDSA SIGN (RFC 6979 - nonce deterministik)
   * ================================================================ */

  function ecdsaSign(messageBytes, privBytes32) {
    if (!privBytes32 || privBytes32.length === 0 || privBytes32.length > 32) {
      throw new Error('ecdsaSign: kunci privat tidak valid');
    }
    var h1 = sha256(messageBytes);
    var x = padScalar32_(privBytes32);
    var d = bytesToModLimbs(x, MONT_N);
    if (limbsIsZero(d)) throw new Error('ecdsaSign: kunci privat nol');

    // bits2octets(h1) = h1 mod n sebagai 32 byte
    var e = bytesToModLimbs(h1, MONT_N); // e = hash mod n

    // --- RFC 6979 ---
    var V = new Array(32).fill(1);
    var K = new Array(32).fill(0);
    var xOct = x.slice();
    var h1Oct = limbsToBytes32(e);

    K = hmacSha256(K, concatBytes(V, [0x00], xOct, h1Oct));
    V = hmacSha256(K, V);
    K = hmacSha256(K, concatBytes(V, [0x01], xOct, h1Oct));
    V = hmacSha256(K, V);

    var r = null, s = null;
    for (var iter = 0; iter < 100; iter++) {
      V = hmacSha256(K, V);
      var vRaw = bytes32ToLimbs(V);
      // RFC 6979: k valid jika 1 <= k < n (tanpa reduksi modulo)
      if (!limbsIsZero(vRaw) && limbsCmp(vRaw, N) < 0) {
        var R = scalarMult(vRaw, { x: GX, y: GY });
        // r = R.x mod n
        var rLimbs = modSub(R.x, N, MONT_N);
        if (!limbsIsZero(rLimbs)) {
          // s = k^-1 * (e + r*d) mod n
          var rd = modMul(rLimbs, d, MONT_N);
          var erd = modAdd(e, rd, MONT_N);
          var kInv = modInv(vRaw, MONT_N);
          var sLimbs = modMul(kInv, erd, MONT_N);
          if (!limbsIsZero(sLimbs)) {
            r = rLimbs; s = sLimbs;
            break;
          }
        }
      }
      K = hmacSha256(K, concatBytes(V, [0x00]));
      V = hmacSha256(K, V);
    }
    if (!r || !s) throw new Error('ecdsaSign: gagal menemukan nonce valid');

    return {
      r: limbsToBytes32(r),
      s: limbsToBytes32(s),
      raw: concatBytes(limbsToBytes32(r), limbsToBytes32(s)) // 64 byte (JWS)
    };
  }

  /* ECDH: privat bytes32 x kunci publik 65 byte -> rahasia bersama 32 byte */
  function ecdhSharedSecret(privBytes32, peerPublicKey65) {
    var d = bytesToModLimbs(padScalar32_(privBytes32), MONT_N);
    if (limbsIsZero(d)) throw new Error('ecdh: kunci privat nol');
    var Q = parsePublicKey(peerPublicKey65);
    var S = scalarMult(d, Q);
    return limbsToBytes32(S.x);
  }

  /* ================================================================
   * 5. AES-128 + GCM
   * ================================================================ */

  /* S-box dihitung programatik (invers GF(2^8) + transformasi afin) */
  var AES_SBOX = (function () {
    var sbox = new Array(256);
    var exp = new Array(255), log = new Array(256);
    var x = 1;
    for (var i = 0; i < 255; i++) {
      exp[i] = x;
      log[x] = i;
      // x *= 3 di GF(2^8): x ^ xtime(x)
      var xt = (x << 1) ^ ((x & 0x80) ? 0x11b : 0);
      x = (x ^ xt) & 0xff;
    }
    for (var b = 0; b < 256; b++) {
      var inv = (b === 0) ? 0 : exp[(255 - log[b]) % 255];
      var res = inv;
      var rot = inv;
      for (var r = 0; r < 4; r++) {
        rot = ((rot << 1) | (rot >>> 7)) & 0xff;
        res ^= rot;
      }
      sbox[b] = res ^ 0x63;
    }
    // verifikasi nilai yang dikenal luas
    if (sbox[0x00] !== 0x63 || sbox[0x01] !== 0x7c || sbox[0x53] !== 0xed || sbox[0xff] !== 0x16) {
      throw new Error('AES S-box self-check gagal');
    }
    return sbox;
  })();

  function aesExpandKey(keyBytes16) {
    if (keyBytes16.length !== 16) throw new Error('AES-128: kunci 16 byte');
    var w = new Array(44); // 44 word 32-bit
    for (var i = 0; i < 4; i++) {
      w[i] = (keyBytes16[i * 4] << 24) | (keyBytes16[i * 4 + 1] << 16) |
             (keyBytes16[i * 4 + 2] << 8) | keyBytes16[i * 4 + 3];
    }
    var rcon = 1;
    for (var t = 4; t < 44; t++) {
      var temp = w[t - 1];
      if (t % 4 === 0) {
        // SubWord + RotWord + Rcon
        var b0 = (temp >>> 24) & 0xff, b1 = (temp >>> 16) & 0xff,
            b2 = (temp >>> 8) & 0xff, b3 = temp & 0xff;
        temp = ((AES_SBOX[b1] << 24) | (AES_SBOX[b2] << 16) |
                (AES_SBOX[b3] << 8) | AES_SBOX[b0]);
        temp = (temp ^ (rcon << 24)) | 0;
        rcon = ((rcon << 1) ^ ((rcon & 0x80) ? 0x11b : 0)) & 0xff;
      }
      w[t] = (w[t - 4] ^ temp) | 0;
    }
    return w;
  }

  function aesEncryptBlock(block16, w) {
    var s = block16.slice();

    function addRoundKey(round) {
      for (var c = 0; c < 4; c++) {
        var word = w[round * 4 + c];
        s[c * 4] ^= (word >>> 24) & 0xff;
        s[c * 4 + 1] ^= (word >>> 16) & 0xff;
        s[c * 4 + 2] ^= (word >>> 8) & 0xff;
        s[c * 4 + 3] ^= word & 0xff;
      }
    }

    addRoundKey(0);
    for (var round = 1; round <= 9; round++) {
      // SubBytes
      for (var i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
      // ShiftRows (state kolom-major: s[c*4+r])
      var t;
      // baris 1 geser 1
      t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
      // baris 2 geser 2
      t = s[2]; s[2] = s[10]; s[10] = t;
      t = s[6]; s[6] = s[14]; s[14] = t;
      // baris 3 geser 3
      t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
      // MixColumns
      for (var c2 = 0; c2 < 4; c2++) {
        var a0 = s[c2 * 4], a1 = s[c2 * 4 + 1], a2 = s[c2 * 4 + 2], a3 = s[c2 * 4 + 3];
        s[c2 * 4]     = gf2(a0) ^ gf3(a1) ^ a2 ^ a3;
        s[c2 * 4 + 1] = a0 ^ gf2(a1) ^ gf3(a2) ^ a3;
        s[c2 * 4 + 2] = a0 ^ a1 ^ gf2(a2) ^ gf3(a3);
        s[c2 * 4 + 3] = gf3(a0) ^ a1 ^ a2 ^ gf2(a3);
      }
      addRoundKey(round);
    }
    // Round final tanpa MixColumns
    for (var i2 = 0; i2 < 16; i2++) s[i2] = AES_SBOX[s[i2]];
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
    addRoundKey(10);
    return s;
  }

  function gf2(x) { return ((x << 1) ^ ((x & 0x80) ? 0x1b : 0)) & 0xff; }
  function gf3(x) { return (gf2(x) ^ x) & 0xff; }

  /* ---- GCM (SP 800-38D) ---- */

  function blockToWords(b16) {
    return [(b16[0] << 24) | (b16[1] << 16) | (b16[2] << 8) | b16[3],
            (b16[4] << 24) | (b16[5] << 16) | (b16[6] << 8) | b16[7],
            (b16[8] << 24) | (b16[9] << 16) | (b16[10] << 8) | b16[11],
            (b16[12] << 24) | (b16[13] << 16) | (b16[14] << 8) | b16[15]];
  }

  function wordsToBlock(w) {
    var out = [];
    for (var i = 0; i < 4; i++) {
      out.push((w[i] >>> 24) & 0xff, (w[i] >>> 16) & 0xff, (w[i] >>> 8) & 0xff, w[i] & 0xff);
    }
    return out;
  }

  /* Perkalian GF(2^128) GCM: Z = X • Y */
  function gcmMult(xw, yw) {
    var zw = [0, 0, 0, 0];
    var vw = yw.slice();
    // R = 0xE1 || 0^120 -> word pertama 0xE1000000
    for (var i = 0; i < 128; i++) {
      // bit i dari X (i=0 = MSB word0)
      var wordIdx = i >> 5, bitIdx = 31 - (i & 31);
      if ((xw[wordIdx] >>> bitIdx) & 1) {
        zw[0] ^= vw[0]; zw[1] ^= vw[1]; zw[2] ^= vw[2]; zw[3] ^= vw[3];
      }
      // V >>= 1 (LSB = bit0 dari word3)
      var lsb = vw[3] & 1;
      vw[3] = (vw[3] >>> 1) | ((vw[2] & 1) << 31);
      vw[2] = (vw[2] >>> 1) | ((vw[1] & 1) << 31);
      vw[1] = (vw[1] >>> 1) | ((vw[0] & 1) << 31);
      vw[0] = vw[0] >>> 1;
      if (lsb) vw[0] ^= 0xE1000000;
    }
    return zw;
  }

  function gcmEncrypt(keyBytes16, ivBytes12, plaintextBytes) {
    var w = aesExpandKey(keyBytes16);
    var H = blockToWords(aesEncryptBlock([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], w));

    // J0 = IV || 0x00000001
    var j0 = ivBytes12.concat([0, 0, 0, 1]);

    function inc32(block) {
      // Inkremen 32-bit terakhir dari blok 16 byte (byte 12..15)
      var c = block.length - 1;
      do {
        block[c] = (block[c] + 1) & 0xff;
        if (block[c] !== 0) break;
        c--;
      } while (c >= block.length - 4);
    }

    function gctr(counterBlock, data) {
      var out = [];
      var ctr = counterBlock.slice();
      for (var off = 0; off < data.length; off += 16) {
        var ks = aesEncryptBlock(ctr, w);
        var n = Math.min(16, data.length - off);
        for (var i = 0; i < n; i++) out.push(data[off + i] ^ ks[i]);
        inc32(ctr);
      }
      return out;
    }

    // ICB = J0 + 1 untuk blok data pertama
    var icb = j0.slice();
    inc32(icb);
    var ciphertext = gctr(icb, plaintextBytes);

    // GHASH: A (kosong) || C || len block
    var y = [0, 0, 0, 0];
    var blocks = [];
    // blok ciphertext
    for (var off = 0; off < ciphertext.length; off += 16) {
      var blk = ciphertext.slice(off, off + 16);
      while (blk.length < 16) blk.push(0);
      blocks.push(blk);
    }
    // blok panjang: len(A) 64-bit || len(C) 64-bit (bit)
    var aLenBits = 0; // AAD kosong
    var cLenBits = plaintextBytes.length * 8;
    var lenBlock = [];
    for (var i = 7; i >= 0; i--) lenBlock.push((aLenBits / Math.pow(2, i * 8)) & 0xff);
    for (var i2 = 7; i2 >= 0; i2--) lenBlock.push((cLenBits / Math.pow(2, i2 * 8)) & 0xff);
    blocks.push(lenBlock);

    for (var bi = 0; bi < blocks.length; bi++) {
      var xb = blockToWords(blocks[bi]);
      y[0] ^= xb[0]; y[1] ^= xb[1]; y[2] ^= xb[2]; y[3] ^= xb[3];
      y = gcmMult(y, H);
    }

    // T = E(K, J0) XOR S
    var ej0 = blockToWords(aesEncryptBlock(j0, w));
    var tag = wordsToBlock([ej0[0] ^ y[0], ej0[1] ^ y[1], ej0[2] ^ y[2], ej0[3] ^ y[3]]);
    return { ciphertext: ciphertext, tag: tag };
  }

  /* ================================================================
   * 6. VAPID (RFC 8292)
   * ================================================================ */

  /**
   * Bangun header VAPID untuk sebuah endpoint push.
   * @param {string} endpoint      URL langganan push
   * @param {string} privKeyB64url kunci privat VAPID (base64url, 32 byte)
   * @param {string} pubKeyB64url  kunci publik VAPID (base64url, 65 byte)
   * @param {string} subject       kontak, mis. "mailto:admin@contoh.id"
   * @param {number} expiresSec    masa berlaku JWT (detik), default 12 jam
   * @returns {{Authorization:string, aud:string, exp:number}}
   */
  function vapidHeaders(endpoint, privKeyB64url, pubKeyB64url, subject, expiresSec) {
    var m = /^(\w+:\/\/[^\/]+)/.exec(endpoint);
    if (!m) throw new Error('vapid: endpoint tidak valid');
    var aud = m[1]; // origin layanan push
    var now = Math.floor(Date.now() / 1000);
    var exp = now + (expiresSec || 12 * 3600);

    var header = b64urlEncode(utf8Encode(JSON.stringify({ typ: 'JWT', alg: 'ES256' })));
    var payload = b64urlEncode(utf8Encode(JSON.stringify({
      aud: aud, exp: exp, sub: subject
    })));
    var signingInput = header + '.' + payload;
    var sig = ecdsaSign(utf8Encode(signingInput), b64urlDecode(privKeyB64url));
    var jwt = signingInput + '.' + b64urlEncode(sig.raw);

    return {
      Authorization: 'vapid t=' + jwt + ', k=' + pubKeyB64url,
      aud: aud,
      exp: exp
    };
  }

  /* ================================================================
   * 7. ENKRIPSI PAYLOAD (RFC 8291 + RFC 8188)
   * ================================================================ */

  /**
   * Enkripsi payload untuk dikirim ke langganan push.
   * @param {object} subscription { endpoint, keys: { p256dh, auth } }
   * @param {string} payloadString  teks payload (JSON kecil)
   * @param {object} [opts] { salt:bytes16, asPrivateKey:bytes32 }
   * @returns {{ body:Array<byte>, headers:Object }}
   */
  function encryptPayload(subscription, payloadString, opts) {
    opts = opts || {};
    var uaPub = b64urlDecode(subscription.keys.p256dh);   // 65 byte
    var authSecret = b64urlDecode(subscription.keys.auth); // 16 byte
    if (uaPub.length !== 65) throw new Error('p256dh harus 65 byte');
    if (authSecret.length !== 16) throw new Error('auth harus 16 byte');

    var salt = opts.salt || randomBytes(16);
    if (salt.length !== 16) throw new Error('salt harus 16 byte');

    // Kunci efemeral aplikasi (as)
    var asPriv = opts.asPrivateKey || randomBytes(32);
    asPriv = padScalar32_(asPriv); // terima skalar pendek (temuan X-8)
    var d = bytesToModLimbs(asPriv, MONT_N);
    if (limbsIsZero(d)) throw new Error('kunci privat as nol');
    var asPub = publicKeyFromPrivate(asPriv);

    // 1) ECDH antara kunci efemeral dan kunci langganan browser
    var ecdhSecret = ecdhSharedSecret(asPriv, uaPub);

    // 2) IKM via HKDF
    var keyInfo = concatBytes(utf8Encode('WebPush: info'), [0], uaPub, asPub);
    var ikm = hkdf(authSecret, ecdhSecret, keyInfo, 32);

    // 3) CEK & nonce
    var cek = hkdf(salt, ikm, utf8Encode('Content-Encoding: aes128gcm\0'), 16);
    var nonce = hkdf(salt, ikm, utf8Encode('Content-Encoding: nonce\0'), 12);

    // 4) Enkripsi AES-128-GCM, plaintext = payload || 0x02 (rekord tunggal)
    var plaintext = concatBytes(utf8Encode(payloadString), [0x02]);
    var enc = gcmEncrypt(cek, nonce, plaintext);

    // 5) Body aes128gcm (RFC 8188 par.2 + RFC 8291 par.3.3):
    //    salt(16) || rs(4 BE) || idlen(1)=65 || asPub(65) || ct || tag
    //    Kunci publik efemeral WAJIB berada di header keyid - tanpa
    //    itu browser tidak dapat menghitung rahasia bersama ECDH.
    var rs = 4096;
    var rsBytes = [(rs >>> 24) & 0xff, (rs >>> 16) & 0xff, (rs >>> 8) & 0xff, rs & 0xff];
    var body = concatBytes(salt, rsBytes, [65], asPub, enc.ciphertext, enc.tag);

    return {
      body: body,
      headers: {
        'Content-Encoding': 'aes128gcm',
        'Content-Type': 'application/octet-stream'
      },
      salt: salt,
      asPublicKey: asPub
    };
  }

  /* ================================================================
   * API publik
   * ================================================================ */

  return {
    sha256: sha256,
    hmacSha256: hmacSha256,
    hkdf: hkdf,
    b64urlEncode: b64urlEncode,
    b64urlDecode: b64urlDecode,
    utf8Encode: utf8Encode,
    utf8Decode: utf8Decode,
    concatBytes: concatBytes,
    bytesToHex: bytesToHex,
    randomBytes: randomBytes,
    ecdsaSign: ecdsaSign,
    ecdhSharedSecret: ecdhSharedSecret,
    publicKeyFromPrivate: publicKeyFromPrivate,
    parsePublicKey: parsePublicKey,
    vapidHeaders: vapidHeaders,
    encryptPayload: encryptPayload,
    gcmEncrypt: gcmEncrypt,
    // ekspor internal untuk pengujian:
    _internal: {
      sha256: sha256, hmacSha256: hmacSha256, hkdf: hkdf,
      gcmEncrypt: gcmEncrypt, ecdsaSign: ecdsaSign,
      montMul: montMul, modMul: modMul, modAdd: modAdd, modSub: modSub,
      modInv: modInv, modExp: modExp,
      MONT_P: MONT_P, MONT_N: MONT_N, P: P, N: N,
      bytes32ToLimbs: bytes32ToLimbs, limbsToBytes32: limbsToBytes32,
      bytesToModLimbs: bytesToModLimbs, scalarMult: scalarMult,
      pointDouble: pointDouble, pointAddMixed: pointAddMixed,
      aesEncryptBlock: aesEncryptBlock, aesExpandKey: aesExpandKey,
      AES_SBOX: AES_SBOX
    }
  };
})();

/* Ekspor untuk Node.js (pengujian); diabaikan di GAS. */
if (typeof module !== 'undefined' && module.exports) {
  module.exports = WebPushCore;
}
