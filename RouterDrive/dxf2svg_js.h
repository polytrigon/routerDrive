// Auto-generated from dxf2svg.js - do not hand-edit, regenerate instead.
// Served at GET /dxf2svg.js by web_server.cpp.
#pragma once

static const char DXF2SVG_JS_SOURCE[] PROGMEM = R"DXFJS(/*
 * RouterDrive - DXF to SVG converter (runs in the browser, before upload).
 *
 * Scope, deliberately: LINE, ARC, CIRCLE, LWPOLYLINE (incl. bulge arcs) and
 * SPLINE entities. Text placed with Onshape's Text tool and exported
 * directly - no manual "explode" step needed - already comes out as this
 * kind of plain line/arc geometry (confirmed against a real Onshape
 * export), so ordinary text just works. What's still NOT supported is a
 * native DXF TEXT/MTEXT entity - the kind that references a font instead
 * of carrying its own outline geometry, which some other CAD tools may
 * still export - those are silently dropped, since there's no font
 * renderer here. If your tool exports text that way, convert it to
 * outline curves first (Onshape calls this "explode" or "convert to
 * sketch geometry"), or upload a finished SVG directly if a design needs
 * real lettering.
 *
 * Design choice: every curve (ARC, CIRCLE, LWPOLYLINE bulge segments,
 * SPLINE) is tessellated into a plain polyline rather than emitted as a
 * native SVG <path> arc/curve command. This avoids a whole class of sign
 * mistakes around SVG's arc sweep-flag convention interacting with the
 * DXF-Y-up -> SVG-Y-down flip - a wrong flag there silently mirrors an arc.
 * Tessellation error is bounded well under CNC-relevant tolerances (see
 * MAX_CHORD_ERROR_MM below).
 *
 * Each DXF entity converts to its own polyline first, then a stitching
 * pass (joinPolylines) glues polylines whose endpoints coincide into
 * single continuous <path>s, closing the loop with an SVG "Z" wherever a
 * chain comes back around to its own start. Without this, an outline
 * drawn as alternating LINE/ARC entities (a very common way to draw a
 * rounded-corner panel) would render as a pile of disconnected segments
 * instead of one cuttable path, even though every endpoint lines up
 * exactly in the source file.
 *
 * No external dependencies (a real npm registry wasn't reachable from the
 * environment this was built in, and for the trickiest part - splines -
 * hand-rolling and testing our own de Boor evaluation was safer than
 * blindly trusting an unverifiable third-party bundle anyway).
 */
(function (global) {
  'use strict';

  var MAX_CHORD_ERROR_MM = 0.05; // tessellation tolerance for arcs/circles/bulges
  var MAX_ARC_STEP_RAD = (10 * Math.PI) / 180; // never take steps coarser than 10 deg

  // Onshape's DXF export always ships this ~40-entry layer table, but only
  // ever puts real cut geometry on a couple of them (VISIBLE, or "0"). The
  // rest are annotation/reference layers - matched here by substring so we
  // don't have to enumerate Onshape's exact list.
  var EXCLUDE_LAYER_SUBSTRINGS = [
    'CENTERLINE', 'CENTERMARK', 'DIMENSION', 'ANNOTATION', 'BORDER',
    'HATCH', 'THREAD', 'EXPLODE', 'BREAK', 'BEND', 'FORM', 'REGION_ASSOC',
    'TABLES', 'IMAGES', 'SECTION_CUTTING',
  ];

  function isLayerExcluded(layer) {
    if (!layer) return false;
    var upper = layer.toUpperCase();
    for (var i = 0; i < EXCLUDE_LAYER_SUBSTRINGS.length; i++) {
      if (upper.indexOf(EXCLUDE_LAYER_SUBSTRINGS[i]) !== -1) return true;
    }
    return false;
  }

  // ---------------------------------------------------------------------
  // DXF group-code tokenizer + entity grouping
  // ---------------------------------------------------------------------

  function tokenize(text) {
    var lines = text.split(/\r\n|\r|\n/);
    var tokens = [];
    for (var i = 0; i + 1 < lines.length; i += 2) {
      var code = parseInt(lines[i].trim(), 10);
      var value = lines[i + 1].trim();
      if (isNaN(code)) continue;
      tokens.push({ code: code, value: value });
    }
    return tokens;
  }

  function findSection(tokens, name) {
    var start = -1;
    for (var i = 0; i < tokens.length; i++) {
      if (tokens[i].code === 2 && tokens[i].value === name) {
        start = i + 1;
        break;
      }
    }
    if (start === -1) return null;
    var end = tokens.length;
    for (var j = start; j < tokens.length; j++) {
      if (tokens[j].code === 0 && tokens[j].value === 'ENDSEC') {
        end = j;
        break;
      }
    }
    return { start: start, end: end };
  }

  function groupEntities(tokens, start, end) {
    var entities = [];
    var cur = null;
    for (var i = start; i < end; i++) {
      var t = tokens[i];
      if (t.code === 0) {
        if (cur) entities.push(cur);
        cur = { type: t.value, codes: [] };
      } else if (cur) {
        cur.codes.push(t);
      }
    }
    if (cur) entities.push(cur);
    return entities;
  }

  function getCode(codes, code, nth) {
    nth = nth || 0;
    var n = 0;
    for (var i = 0; i < codes.length; i++) {
      if (codes[i].code === code) {
        if (n === nth) return codes[i].value;
        n++;
      }
    }
    return undefined;
  }

  function getAllCodes(codes, code) {
    var out = [];
    for (var i = 0; i < codes.length; i++) {
      if (codes[i].code === code) out.push(codes[i].value);
    }
    return out;
  }

  // ---------------------------------------------------------------------
  // Curve tessellation
  // ---------------------------------------------------------------------

  function arcStep(radius) {
    var r = Math.max(radius, 1e-6);
    var cosArg = Math.max(-1, Math.min(1, 1 - MAX_CHORD_ERROR_MM / r));
    var step = 2 * Math.acos(cosArg);
    if (!isFinite(step) || step <= 0) step = MAX_ARC_STEP_RAD;
    return Math.min(step, MAX_ARC_STEP_RAD);
  }

  // Sweep points around (cx,cy) radius r, starting at angle a1 (radians),
  // through a SIGNED angle thetaSigned (positive = CCW, negative = CW).
  // Returns points including both endpoints, in raw DXF (Y-up) space. The
  // final point is snapped exactly to the analytic endpoint so callers can
  // rely on closed loops staying exactly closed despite float rounding.
  function sweepPoints(cx, cy, r, a1, thetaSigned) {
    var step = arcStep(r);
    var n = Math.max(2, Math.ceil(Math.abs(thetaSigned) / step));
    var pts = [];
    for (var k = 0; k <= n; k++) {
      var a = a1 + (thetaSigned * k) / n;
      pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]);
    }
    return pts;
  }

  // Points for a DXF-style ARC/CIRCLE: center (cx,cy), radius r, sweeping
  // counter-clockwise from a1 to a2 (degrees) - DXF's ARC entity is always
  // CCW from its start angle to its end angle, wrapping through 360 if
  // a2 < a1. Returns points including both endpoints.
  function arcPoints(cx, cy, r, a1Deg, a2Deg) {
    var a1 = (a1Deg * Math.PI) / 180;
    var a2 = (a2Deg * Math.PI) / 180;
    var sweep = a2 - a1;
    while (sweep <= 0) sweep += 2 * Math.PI;
    return sweepPoints(cx, cy, r, a1, sweep);
  }

  // LWPOLYLINE bulge segment from (x1,y1) to (x2,y2). Returns points
  // AFTER the start point, up to and including the end point (so segments
  // can be concatenated directly). bulge === 0 gives a straight line.
  //
  // Center is solved via complex-number rotation rather than a perpendicular
  // chord offset: DXF defines bulge = tan(theta/4), where theta is the
  // SIGNED angle (positive = CCW) swept about the arc's center going from
  // point 1 to point 2. That is exactly the statement "z2 equals z1 rotated
  // by theta about the center", i.e. z2 = c + (z1 - c)*e^{i*theta}, which
  // solves in closed form for c with no case-splitting on theta's sign or
  // magnitude - avoiding the sign mistakes that a geometric offset-distance
  // construction is prone to. Verified against known-good constraints
  // (both endpoints equidistant from the solved center; two opposing 180 deg
  // bulges reproducing an exact circle to ~5e-4mm) before relying on it.
  function bulgeSegmentPoints(x1, y1, x2, y2, bulge) {
    if (!bulge) return [[x2, y2]];
    var theta = 4 * Math.atan(bulge); // signed: matches DXF's CCW-positive convention
    var cosT = Math.cos(theta), sinT = Math.sin(theta);
    // z1 * e^{i theta}
    var z1rx = x1 * cosT - y1 * sinT;
    var z1ry = x1 * sinT + y1 * cosT;
    // c = (z2 - z1*rot) / (1 - rot)
    var numX = x2 - z1rx, numY = y2 - z1ry;
    var denX = 1 - cosT, denY = -sinT;
    var denMagSq = denX * denX + denY * denY;
    if (denMagSq < 1e-18) return [[x2, y2]]; // theta ~ 0, degenerate
    var cx = (numX * denX + numY * denY) / denMagSq;
    var cy = (numY * denX - numX * denY) / denMagSq;
    var r = Math.hypot(x1 - cx, y1 - cy);
    var a1 = Math.atan2(y1 - cy, x1 - cx);
    var pts = sweepPoints(cx, cy, r, a1, theta);
    pts[pts.length - 1] = [x2, y2]; // snap to the exact analytic endpoint
    return pts.slice(1); // drop the duplicate start point
  }

  // ---------------------------------------------------------------------
  // B-spline (NURBS) evaluation - Cox-de Boor
  // ---------------------------------------------------------------------

  function findSpan(n, degree, t, knots) {
    if (t >= knots[n + 1]) return n;
    if (t <= knots[degree]) return degree;
    var lo = degree, hi = n + 1;
    while (t < knots[lo] || t >= knots[lo + 1]) {
      var mid = Math.floor((lo + hi) / 2);
      if (t < knots[mid]) hi = mid; else lo = mid;
    }
    return lo;
  }

  function deBoorPoint(t, degree, knots, ctrlPts, weights) {
    var n = ctrlPts.length - 1;
    var k = findSpan(n, degree, t, knots);
    var d = [];
    for (var j = 0; j <= degree; j++) {
      var idx = k - degree + j;
      var w = weights[idx];
      d[j] = { x: ctrlPts[idx][0] * w, y: ctrlPts[idx][1] * w, w: w };
    }
    for (var r = 1; r <= degree; r++) {
      for (var jj = degree; jj >= r; jj--) {
        var i = k - degree + jj;
        var denom = knots[i + degree - r + 1] - knots[i];
        var alpha = denom === 0 ? 0 : (t - knots[i]) / denom;
        d[jj] = {
          x: (1 - alpha) * d[jj - 1].x + alpha * d[jj].x,
          y: (1 - alpha) * d[jj - 1].y + alpha * d[jj].y,
          w: (1 - alpha) * d[jj - 1].w + alpha * d[jj].w,
        };
      }
    }
    var last = d[degree];
    return [last.x / last.w, last.y / last.w];
  }

  function splinePoints(degree, knots, ctrlPts, weights, samplesPerSpan) {
    samplesPerSpan = samplesPerSpan || 12;
    var n = ctrlPts.length - 1;
    var tMin = knots[degree], tMax = knots[n + 1];
    var spans = 0;
    for (var i = degree; i <= n; i++) {
      if (knots[i + 1] > knots[i]) spans++;
    }
    var totalSamples = Math.max(8, Math.min(400, spans * samplesPerSpan));
    var pts = [];
    for (var s = 0; s <= totalSamples; s++) {
      var t = tMin + ((tMax - tMin) * s) / totalSamples;
      if (s === totalSamples) t = tMax; // avoid float overshoot past domain
      pts.push(deBoorPoint(t, degree, knots, ctrlPts, weights));
    }
    return pts;
  }

  // ---------------------------------------------------------------------
  // Entity -> polyline extraction
  // ---------------------------------------------------------------------

  function extractPolylines(entities) {
    var polylines = [];
    var skipped = {};

    entities.forEach(function (e) {
      var layer = getCode(e.codes, 8);
      if (isLayerExcluded(layer)) {
        skipped[e.type] = (skipped[e.type] || 0) + 1;
        return;
      }

      if (e.type === 'LINE') {
        var x1 = parseFloat(getCode(e.codes, 10)), y1 = parseFloat(getCode(e.codes, 20));
        var x2 = parseFloat(getCode(e.codes, 11)), y2 = parseFloat(getCode(e.codes, 21));
        polylines.push([[x1, y1], [x2, y2]]);
      } else if (e.type === 'ARC') {
        var cx = parseFloat(getCode(e.codes, 10)), cy = parseFloat(getCode(e.codes, 20));
        var r = parseFloat(getCode(e.codes, 40));
        var a1 = parseFloat(getCode(e.codes, 50)), a2 = parseFloat(getCode(e.codes, 51));
        polylines.push(arcPoints(cx, cy, r, a1, a2));
      } else if (e.type === 'CIRCLE') {
        var ccx = parseFloat(getCode(e.codes, 10)), ccy = parseFloat(getCode(e.codes, 20));
        var cr = parseFloat(getCode(e.codes, 40));
        polylines.push(arcPoints(ccx, ccy, cr, 0, 360));
      } else if (e.type === 'LWPOLYLINE') {
        var verts = [];
        var cur = null;
        for (var i = 0; i < e.codes.length; i++) {
          var c = e.codes[i];
          if (c.code === 10) {
            cur = { x: parseFloat(c.value), y: 0, bulge: 0 };
            verts.push(cur);
          } else if (c.code === 20 && cur) {
            cur.y = parseFloat(c.value);
          } else if (c.code === 42 && cur) {
            cur.bulge = parseFloat(c.value);
          }
        }
        var flags = parseInt(getCode(e.codes, 70) || '0', 10);
        var closed = (flags & 1) === 1;
        if (verts.length >= 2) {
          var poly = [[verts[0].x, verts[0].y]];
          for (var v = 0; v < verts.length - 1; v++) {
            poly = poly.concat(bulgeSegmentPoints(verts[v].x, verts[v].y, verts[v + 1].x, verts[v + 1].y, verts[v].bulge));
          }
          if (closed) {
            var last = verts[verts.length - 1], first = verts[0];
            poly = poly.concat(bulgeSegmentPoints(last.x, last.y, first.x, first.y, last.bulge));
          }
          polylines.push(poly);
        }
      } else if (e.type === 'SPLINE') {
        var degree = parseInt(getCode(e.codes, 71) || '3', 10);
        var knots = getAllCodes(e.codes, 40).map(parseFloat);
        var xs = getAllCodes(e.codes, 10).map(parseFloat);
        var ys = getAllCodes(e.codes, 20).map(parseFloat);
        var ws = getAllCodes(e.codes, 41).map(parseFloat);
        var ctrlPts = xs.map(function (x, i) { return [x, ys[i]]; });
        var weights = ctrlPts.map(function (_, i) { return ws[i] !== undefined && !isNaN(ws[i]) ? ws[i] : 1; });
        if (degree >= 1 && ctrlPts.length > degree && knots.length === ctrlPts.length + degree + 1) {
          polylines.push(splinePoints(degree, knots, ctrlPts, weights));
        } else {
          skipped['SPLINE (malformed)'] = (skipped['SPLINE (malformed)'] || 0) + 1;
        }
      } else if (e.type === 'TEXT' || e.type === 'MTEXT') {
        skipped[e.type] = (skipped[e.type] || 0) + 1;
      } else {
        skipped[e.type] = (skipped[e.type] || 0) + 1;
      }
    });

    return { polylines: polylines, skipped: skipped };
  }

  // ---------------------------------------------------------------------
  // Chain stitching - join separate entities (LINE/ARC/etc, each its own
  // polyline so far) into continuous paths wherever one's endpoint meets
  // another's, and detect closed loops. Without this, a boundary drawn as
  // alternating LINE/ARC entities (very common - e.g. a rounded-corner
  // panel outline) would come out as a pile of disconnected segments
  // instead of one continuous cut path, even though every endpoint lines
  // up exactly. CIRCLE entities already close themselves as single
  // polylines and just pass through unchanged.
  // ---------------------------------------------------------------------

  var JOIN_TOL_MM = 0.01; // generous vs. MAX_CHORD_ERROR_MM (0.05) tessellation error

  function pointsClose(a, b, tol) {
    return Math.hypot(a[0] - b[0], a[1] - b[1]) <= tol;
  }

  function joinPolylines(polylines, tol) {
    tol = tol || JOIN_TOL_MM;
    var remaining = polylines;
    var used = new Array(remaining.length).fill(false);
    var chains = [];

    for (var start = 0; start < remaining.length; start++) {
      if (used[start]) continue;
      used[start] = true;
      var chain = remaining[start].slice();

      // Repeatedly try to extend the chain's end, then its start, with any
      // still-unused polyline (in either direction/orientation). O(n^2) in
      // the worst case, which is fine for the entity counts real DXF
      // exports have (tens to low hundreds).
      while (true) {
        var extended = false;
        var chainEnd = chain[chain.length - 1];
        for (var j = 0; j < remaining.length && !extended; j++) {
          if (used[j]) continue;
          var poly = remaining[j];
          if (pointsClose(chainEnd, poly[0], tol)) {
            chain = chain.concat(poly.slice(1));
            used[j] = true;
            extended = true;
          } else if (pointsClose(chainEnd, poly[poly.length - 1], tol)) {
            chain = chain.concat(poly.slice(0, -1).reverse());
            used[j] = true;
            extended = true;
          }
        }
        if (extended) continue;

        var chainStart = chain[0];
        for (var k = 0; k < remaining.length && !extended; k++) {
          if (used[k]) continue;
          var poly2 = remaining[k];
          if (pointsClose(chainStart, poly2[poly2.length - 1], tol)) {
            chain = poly2.slice(0, -1).concat(chain);
            used[k] = true;
            extended = true;
          } else if (pointsClose(chainStart, poly2[0], tol)) {
            chain = poly2.slice(1).reverse().concat(chain);
            used[k] = true;
            extended = true;
          }
        }
        if (!extended) break;
      }

      chains.push(chain);
    }
    return chains;
  }

  // ---------------------------------------------------------------------
  // SVG generation
  // ---------------------------------------------------------------------

  function fmt(n) {
    var s = n.toFixed(3);
    s = s.replace(/0+$/, '').replace(/\.$/, '');
    return s === '' || s === '-' ? '0' : s;
  }

  // opts.outputUnit: 'mm' or 'in'. Source coordinates are always assumed
  // to be millimeters (matches this project's Onshape export convention).
  function toSVG(dxfText, opts) {
    opts = opts || {};
    var outputUnit = opts.outputUnit === 'in' ? 'in' : 'mm';

    var tokens = tokenize(dxfText);
    var section = findSection(tokens, 'ENTITIES');
    if (!section) {
      throw new Error('No ENTITIES section found - is this a valid ASCII DXF file?');
    }
    var entities = groupEntities(tokens, section.start, section.end);
    var result = extractPolylines(entities);
    var polylines = result.polylines;

    if (polylines.length === 0) {
      throw new Error('No usable geometry found (after filtering annotation layers/text). ' +
        'If this design uses entity types outside LINE/ARC/CIRCLE/LWPOLYLINE/SPLINE, it needs manual conversion.');
    }

    var chains = joinPolylines(polylines);

    var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    chains.forEach(function (poly) {
      poly.forEach(function (p) {
        if (p[0] < minX) minX = p[0];
        if (p[0] > maxX) maxX = p[0];
        if (p[1] < minY) minY = p[1];
        if (p[1] > maxY) maxY = p[1];
      });
    });

    var widthMM = maxX - minX;
    var heightMM = maxY - minY;

    // Y-flip (DXF Y-up -> SVG Y-down) + shift to origin.
    function transform(p) {
      return [p[0] - minX, maxY - p[1]];
    }

    var pathElements = chains.map(function (poly) {
      var tpts = poly.map(transform);
      // If the chain came back around to (near) its own start: rather than
      // relying solely on SVG's "Z" close-path command to signal a closed
      // loop, explicitly snap the final point to be bit-identical to the
      // first (dropping whatever near-duplicate point the join tolerance
      // left there) - so the path is unambiguously closed to any importer
      // that checks "does the vertex list return to its start" rather than
      // parsing Z semantics, in addition to Z itself for standards-compliant
      // renderers. Costs nothing and can only help compatibility.
      var isClosed = tpts.length > 2 && pointsClose(tpts[0], tpts[tpts.length - 1], JOIN_TOL_MM);
      if (isClosed) {
        tpts = tpts.slice(0, -1);
        tpts.push(tpts[0]);
      }
      var d = 'M ' + tpts.map(function (p) { return fmt(p[0]) + ',' + fmt(p[1]); }).join(' L ');
      if (isClosed) d += ' Z';
      return '<path d="' + d + '" fill="none" stroke="#000" stroke-width="0.1"/>';
    });

    var widthOut = outputUnit === 'in' ? widthMM / 25.4 : widthMM;
    var heightOut = outputUnit === 'in' ? heightMM / 25.4 : heightMM;

    var svg =
      '<svg xmlns="http://www.w3.org/2000/svg" ' +
      'width="' + fmt(widthOut) + outputUnit + '" height="' + fmt(heightOut) + outputUnit + '" ' +
      'viewBox="0 0 ' + fmt(widthMM) + ' ' + fmt(heightMM) + '">\n' +
      pathElements.join('\n') +
      '\n</svg>\n';

    return { svg: svg, widthMM: widthMM, heightMM: heightMM, skipped: result.skipped, pathCount: chains.length };
  }

  global.RouterDriveDXF = { toSVG: toSVG, _internal: { tokenize: tokenize, extractPolylines: extractPolylines, splinePoints: splinePoints, deBoorPoint: deBoorPoint, arcPoints: arcPoints, joinPolylines: joinPolylines } };
})(typeof window !== 'undefined' ? window : globalThis);
)DXFJS";
