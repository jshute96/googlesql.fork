// Full-window visualizer page support.
//
// Two roles, selected by which elements are present on the page:
//
//  1. Main page (#visualize-btn next to Submit): clicking the button serializes
//     the current form, stashes it in localStorage under a fresh key, and opens
//     /visualize?k=<key> in a new tab. The request is kept client-side (not in
//     the URL) so it survives reload without a POST-resubmission prompt and is
//     not limited by URL length.
//
//  2. The /visualize shell page (#visualize-root): on load it reads the stashed
//     request for the ?k= key, POSTs it to /visualize to render the visualizer
//     content, injects the result, and wires the statement dropdown and the
//     pre/post-rewrite toggle. A reload re-reads the same key and re-renders, so
//     it behaves like re-submitting the same script.
(function () {
  'use strict';

  var KEY_PREFIX = 'googlesql-visualize-';

  // ---- Role 1: the "Visualize" button on the main page. -------------------
  var btn = document.getElementById('visualize-btn');
  if (btn) {
    btn.addEventListener('click', function () {
      var form = document.getElementById('form');
      if (!form) return;
      var fd = new FormData(form);
      var params = new URLSearchParams();
      fd.forEach(function (value, name) {
        // The full-window page always visualizes; drop the mode checkboxes.
        if (name === 'mode') return;
        params.append(name, value);
      });
      params.set('mode', 'visualize');
      var key = 'k' + Date.now().toString(36) + '_' +
                Math.floor(Math.random() * 1e9).toString(36);
      try {
        pruneOldRequests();
        localStorage.setItem(KEY_PREFIX + key, params.toString());
      } catch (e) { /* storage may be unavailable; open anyway */ }
      window.open('/visualize?k=' + encodeURIComponent(key), '_blank');
    });
  }

  // Keep localStorage from growing without bound: keep only the newest entries.
  function pruneOldRequests() {
    var keys = [];
    for (var i = 0; i < localStorage.length; i++) {
      var k = localStorage.key(i);
      if (k && k.indexOf(KEY_PREFIX) === 0) keys.push(k);
    }
    if (keys.length < 20) return;
    keys.sort();  // keys embed a timestamp prefix, so this is roughly by age.
    for (var j = 0; j < keys.length - 19; j++) localStorage.removeItem(keys[j]);
  }

  // ---- Role 2: the /visualize shell page. ---------------------------------
  var root = document.getElementById('visualize-root');
  if (!root) return;

  var key = new URLSearchParams(window.location.search).get('k');
  var body = key ? localStorage.getItem(KEY_PREFIX + key) : null;
  if (!body) {
    root.innerHTML = '<div class="visualize-placeholder">No saved query for ' +
        'this visualization. Open it with the <b>Visualize</b> button on the ' +
        '<a href="/">main page</a>.</div>';
    return;
  }

  fetch('/visualize', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: body
  }).then(function (r) {
    return r.text();
  }).then(function (html) {
    root.innerHTML = html;
    // Make the initial statement/rewrite block visible BEFORE initializing the
    // panes, so the viz layout is measured while it is displayed (the column
    // sizing logic in query_viewer.js needs a laid-out, non-hidden block).
    wireControls();
    if (window.googlesqlInitVisualize) window.googlesqlInitVisualize();
  }).catch(function (e) {
    root.innerHTML = '<div class="visualize-placeholder">Failed to load ' +
        'visualization: ' + String(e) + '</div>';
  });

  function setRewrite(stmt, rewriteKey) {
    if (!stmt) return;
    var blocks = stmt.querySelectorAll('.visualize-block');
    for (var i = 0; i < blocks.length; i++) {
      blocks[i].classList.toggle(
          'active', blocks[i].getAttribute('data-rewrite') === rewriteKey);
    }
  }

  function reflow() {
    if (window.googlesqlReflowViz) window.googlesqlReflowViz();
  }

  function wireControls() {
    var select = root.querySelector('#visualize-stmt-select');
    var toggle = root.querySelector('#visualize-rewrite-toggle');
    var statements =
        Array.prototype.slice.call(root.querySelectorAll('.visualize-statement'));
    if (statements.length === 0) return;

    function showStatement(idx, skipReflow) {
      var active = null;
      statements.forEach(function (s) {
        var on = s.getAttribute('data-stmt-index') === String(idx);
        s.classList.toggle('active', on);
        if (on) active = s;
      });
      if (!active) { active = statements[0]; active.classList.add('active'); }
      var hasPost = active.getAttribute('data-has-post') === '1';
      if (toggle) toggle.hidden = !hasPost;
      // Reset to the pre-rewrite view whenever the statement changes.
      setRewrite(active, 'pre');
      if (toggle) {
        var tabs = toggle.querySelectorAll('.viz-rewrite-tab');
        for (var i = 0; i < tabs.length; i++) {
          tabs[i].classList.toggle(
              'active', tabs[i].getAttribute('data-rewrite') === 'pre');
        }
      }
      if (!skipReflow) reflow();
    }

    if (select) {
      select.addEventListener('change', function () {
        showStatement(this.value);
      });
    }
    if (toggle) {
      toggle.addEventListener('click', function (e) {
        var b = e.target.closest ? e.target.closest('.viz-rewrite-tab') : null;
        if (!b) return;
        var rewriteKey = b.getAttribute('data-rewrite');
        var tabs = toggle.querySelectorAll('.viz-rewrite-tab');
        for (var i = 0; i < tabs.length; i++) {
          tabs[i].classList.toggle('active', tabs[i] === b);
        }
        setRewrite(root.querySelector('.visualize-statement.active'), rewriteKey);
        reflow();
      });
    }

    // Initial activation runs before pane init, which performs the reflow.
    showStatement(statements[0].getAttribute('data-stmt-index'),
                  /*skipReflow=*/true);
  }
})();
