// Query-viewer info panel (analyze mode).
//
// The box-formatted query is a tree of nested `.rect` regions. Regions that the
// resolver had info for carry a hidden `.node-info` child holding a `.ni-title`
// and `.ni-body`. Clicking a region opens a floating panel listing that node
// and all enclosing nodes that have info -- innermost first, each outer one
// prefixed with "in" -- and shows the body of the selected node. The selected
// node is also highlighted in the query text. Click elsewhere or press Escape
// to close.
(function () {
  'use strict';

  var popup = null;
  var selectedRect = null;

  function clearSelectedRect() {
    if (selectedRect) {
      selectedRect.classList.remove('np-selected');
      selectedRect = null;
    }
  }

  function closePanel() {
    if (popup) {
      popup.remove();
      popup = null;
    }
    clearSelectedRect();
  }

  function selectRect(rect) {
    clearSelectedRect();
    selectedRect = rect;
    if (rect) rect.classList.add('np-selected');
  }

  // Walks up the `.rect` ancestors of `startRect`, collecting those that have
  // their own `.node-info` (a direct child). Innermost first.
  function collectItems(startRect, root) {
    var items = [];
    var el = startRect;
    while (el && root.contains(el)) {
      if (el.classList && el.classList.contains('rect')) {
        var info = null;
        for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
          if (c.classList && c.classList.contains('node-info')) {
            info = c;
            break;
          }
        }
        if (info) {
          var titleEl = info.querySelector('.ni-title');
          var bodyEl = info.querySelector('.ni-body');
          items.push({
            rect: el,
            title: titleEl ? titleEl.innerHTML : '',
            body: bodyEl ? bodyEl.innerHTML : ''
          });
        }
      }
      el = el.parentElement;
    }
    return items;
  }

  function openPanel(items, pageX, pageY) {
    closePanel();
    popup = document.createElement('div');
    popup.className = 'node-popup';

    var headings = document.createElement('div');
    var body = document.createElement('div');
    body.className = 'np-body';

    function select(index) {
      var lines = headings.children;
      for (var k = 0; k < lines.length; k++) {
        lines[k].classList.toggle('selected', k === index);
      }
      body.innerHTML = items[index].body;
      selectRect(items[index].rect);
    }

    items.forEach(function (item, i) {
      var line = document.createElement('div');
      line.className = 'np-heading';
      var prefix = i === 0 ? '' : '<span class="np-in">in </span>';
      line.innerHTML = prefix + '<span class="np-title">' + item.title +
          '</span>';
      line.addEventListener('click', function (ev) {
        ev.stopPropagation();
        select(i);
      });
      headings.appendChild(line);
    });

    popup.appendChild(headings);
    var hr = document.createElement('div');
    hr.className = 'np-hr';
    popup.appendChild(hr);
    popup.appendChild(body);
    document.body.appendChild(popup);

    // Position near the click, kept within the viewport horizontally.
    var maxLeft = window.scrollX + document.documentElement.clientWidth -
        popup.offsetWidth - 8;
    var left = Math.max(window.scrollX + 4, Math.min(pageX, maxLeft));
    popup.style.left = left + 'px';
    popup.style.top = pageY + 'px';

    select(0);
  }

  document.addEventListener('click', function (e) {
    // Clicks inside the open panel are handled by the heading handlers.
    if (popup && popup.contains(e.target)) return;
    // Inside the Visualize view, clicks are handled by the visualizer module
    // (which routes node info to the bottom details box), not the floating
    // popup.
    if (e.target.closest && e.target.closest('.viz')) {
      closePanel();
      return;
    }
    var root = e.target.closest ?
        e.target.closest('.formatted-sql.boxed') : null;
    if (!root) {
      closePanel();
      return;
    }
    var rect = e.target.closest('.rect');
    if (!rect || !root.contains(rect)) {
      closePanel();
      return;
    }
    var items = collectItems(rect, root);
    if (items.length === 0) {
      closePanel();
      return;
    }
    e.stopPropagation();
    openPanel(items, e.pageX, e.pageY + 6);
  });

  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') closePanel();
  });
})();

// ===========================================================================
// Visualize mode.
//
// Each statement renders a `.viz` block with up to three resizable columns
// (Input SQL, Resolved AST, SQLBuilder SQL) above a resizable details box.
// The set of visible columns, the column widths, and the details-box height
// are LINKED across every `.viz` block on the page: changing one updates all.
// Clicking a node routes its info to that block's details box.  Correspondence
// highlighting between panes is added in Milestone 4.
// ===========================================================================
(function () {
  'use strict';

  var COLS = ['input', 'ast', 'sqlbuilder'];
  var LABELS = {
    input: 'Input SQL',
    ast: 'Resolved AST',
    sqlbuilder: 'SQLBuilder SQL'
  };

  // Linked state shared by all `.viz` blocks.
  var state = {
    hidden: {},                                  // col key -> true
    weights: {input: 1, ast: 1, sqlbuilder: 1},  // flex-grow per column
    infoH: null,                                 // details-box height in px
    lastVH: window.innerHeight
  };

  function vizBlocks() {
    return Array.prototype.slice.call(document.querySelectorAll('.viz'));
  }

  function visibleCols() {
    return COLS.filter(function (k) { return !state.hidden[k]; });
  }

  // One line height (px) of a details box, used to size it in "lines".
  function lineHeightPx(infoEl) {
    var lh = parseFloat(getComputedStyle(infoEl).lineHeight);
    return isNaN(lh) ? 17 : lh;
  }

  function clampInfo(px, infoEl) {
    var minH = lineHeightPx(infoEl) + 8;        // ~1 line + padding
    var maxH = window.innerHeight * 0.5;
    return Math.max(minH, Math.min(px, maxH));
  }

  // Pushes the linked state onto every `.viz` block.
  function applyState() {
    var vis = visibleCols();
    if (vis.length === 0) { vis = ['input']; state.hidden = {}; }

    vizBlocks().forEach(function (viz) {
      var columnsRow = viz.querySelector('.viz-columns');
      var readd = viz.querySelector('.viz-readd-bar');
      var infoEl = viz.querySelector('.viz-info');

      // Column visibility + widths.
      COLS.forEach(function (k) {
        var col = viz.querySelector('.viz-col[data-col="' + k + '"]');
        if (!col) return;
        col.classList.toggle('hidden', !!state.hidden[k]);
        col.style.setProperty('--w', state.weights[k]);
      });

      // Re-place dividers between adjacent visible columns; hide extras.
      var dividers = Array.prototype.slice.call(
          viz.querySelectorAll('.viz-divider'));
      dividers.forEach(function (d) { d.classList.add('hidden'); });
      for (var i = 0; i < vis.length - 1; i++) {
        var d = dividers[i];
        if (!d) break;
        d.classList.remove('hidden');
        d.dataset.left = vis[i];
        d.dataset.right = vis[i + 1];
        var leftCol = viz.querySelector('.viz-col[data-col="' + vis[i] + '"]');
        // Insert the divider right after its left column.
        if (leftCol && leftCol.nextSibling !== d) {
          columnsRow.insertBefore(d, leftCol.nextSibling);
        }
      }

      // Re-add buttons for hidden columns, in canonical order.
      if (readd) {
        readd.innerHTML = '';
        COLS.forEach(function (k) {
          if (!state.hidden[k]) return;
          var b = document.createElement('button');
          b.type = 'button';
          b.className = 'viz-readd';
          b.dataset.col = k;
          b.textContent = '+ ' + LABELS[k];
          readd.appendChild(b);
        });
      }

      // Details-box height.
      if (infoEl) {
        if (state.infoH == null) {
          state.infoH = 10 * lineHeightPx(infoEl) + 12;  // ~10 lines default
        }
        infoEl.style.setProperty('--info-h', clampInfo(state.infoH, infoEl) +
            'px');
      }
    });
  }

  function hideColumn(key) {
    if (visibleCols().length <= 1) return;   // keep at least one
    state.hidden[key] = true;
    applyState();
  }

  function showColumn(key) {
    delete state.hidden[key];
    applyState();
  }

  // --- Column divider drag (linked widths). ---
  var dragDiv = null;
  function startDividerDrag(divider, e) {
    var viz = divider.closest('.viz');
    var leftKey = divider.dataset.left, rightKey = divider.dataset.right;
    var leftCol = viz.querySelector('.viz-col[data-col="' + leftKey + '"]');
    var rightCol = viz.querySelector('.viz-col[data-col="' + rightKey + '"]');
    dragDiv = {
      leftKey: leftKey, rightKey: rightKey,
      startX: e.clientX,
      leftPx: leftCol.getBoundingClientRect().width,
      rightPx: rightCol.getBoundingClientRect().width,
      sumW: state.weights[leftKey] + state.weights[rightKey]
    };
    e.preventDefault();
  }
  function moveDividerDrag(e) {
    if (!dragDiv) return;
    var delta = e.clientX - dragDiv.startX;
    var l = Math.max(30, dragDiv.leftPx + delta);
    var r = Math.max(30, dragDiv.rightPx - delta);
    var total = l + r;
    state.weights[dragDiv.leftKey] = dragDiv.sumW * (l / total);
    state.weights[dragDiv.rightKey] = dragDiv.sumW * (r / total);
    applyState();
  }

  // --- Details-box resizer drag (linked height). ---
  var dragInfo = null;
  function startInfoDrag(resizer, e) {
    var viz = resizer.closest('.viz');
    var infoEl = viz.querySelector('.viz-info');
    dragInfo = {
      infoEl: infoEl,
      startY: e.clientY,
      startH: infoEl.getBoundingClientRect().height
    };
    e.preventDefault();
  }
  function moveInfoDrag(e) {
    if (!dragInfo) return;
    // Dragging the bar up (smaller clientY) grows the box below it.
    var newH = dragInfo.startH + (dragInfo.startY - e.clientY);
    state.infoH = clampInfo(newH, dragInfo.infoEl);
    applyState();
  }

  document.addEventListener('mousedown', function (e) {
    var divider = e.target.closest && e.target.closest('.viz-divider');
    if (divider && !divider.classList.contains('hidden')) {
      startDividerDrag(divider, e);
      return;
    }
    var resizer = e.target.closest && e.target.closest('.viz-info-resizer');
    if (resizer) { startInfoDrag(resizer, e); }
  });
  document.addEventListener('mousemove', function (e) {
    moveDividerDrag(e);
    moveInfoDrag(e);
  });
  document.addEventListener('mouseup', function () {
    dragDiv = null;
    dragInfo = null;
  });

  // --- Hide / re-add clicks. ---
  document.addEventListener('click', function (e) {
    var hide = e.target.closest && e.target.closest('.viz-hide');
    if (hide) {
      var col = hide.closest('.viz-col');
      if (col) hideColumn(col.dataset.col);
      e.stopPropagation();
      return;
    }
    var readd = e.target.closest && e.target.closest('.viz-readd');
    if (readd) {
      showColumn(readd.dataset.col);
      e.stopPropagation();
      return;
    }
    // Node click -> details box + cross-pane correspondence highlight.
    var pane = e.target.closest && e.target.closest('.viz-pane');
    if (pane) { handleNodeClick(pane, e); }
  });

  // Collects the clicked `.rect` and its enclosing `.rect`s that carry a
  // `.node-info`, innermost first (mirrors the analyze-mode popup).
  function collectNodeInfo(startRect, root) {
    var items = [];
    var el = startRect;
    while (el && root.contains(el)) {
      if (el.classList && el.classList.contains('rect')) {
        var info = null;
        for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
          if (c.classList && c.classList.contains('node-info')) {
            info = c; break;
          }
        }
        if (info) {
          var titleEl = info.querySelector('.ni-title');
          var bodyEl = info.querySelector('.ni-body');
          items.push({
            title: titleEl ? titleEl.innerHTML : '',
            body: bodyEl ? bodyEl.innerHTML : ''
          });
        }
      }
      el = el.parentElement;
    }
    return items;
  }

  function renderDetails(viz, items) {
    var infoEl = viz ? viz.querySelector('.viz-info') : null;
    if (!infoEl) return;
    if (!items || items.length === 0) return;
    // Heading stack (innermost first, outer ones prefixed "in") + body of the
    // selected (innermost) node.
    var html = '<div class="np-body-stack">';
    items.forEach(function (item, i) {
      var prefix = i === 0 ? '' : '<span class="np-in">in </span>';
      html += '<div class="np-heading' + (i === 0 ? ' selected' : '') + '">' +
          prefix + '<span class="np-title">' + item.title + '</span></div>';
    });
    html += '</div><div class="np-hr"></div><div class="np-body">' +
        items[0].body + '</div>';
    infoEl.innerHTML = html;
  }

  // --- Cross-reference correspondence model ------------------------------
  // Every correspondence node carries a stable id in `data-node-id`: Resolved
  // AST and SQLBuilder boxes on the `.rscan` element itself; input boxes on a
  // hidden `.ni-ref` marker inside the box's own `.node-info` (the box formatter
  // owns the `.rect`, so we cannot set an attribute on it directly).  A node's
  // `data-corresp` lists the ids it links to directly (one hop) in the same or
  // an adjacent pane; transitive links (e.g. input -> SQLBuilder via the
  // Resolved AST) are resolved on the fly by walking these edges as undirected.

  // The `.ni-ref` marker carried by an input box's own `.node-info`, if any.
  function markerOfRect(rect) {
    if (!rect || !rect.classList || !rect.classList.contains('rect')) return null;
    for (var c = rect.firstElementChild; c; c = c.nextElementSibling) {
      if (c.classList && c.classList.contains('node-info')) {
        return c.querySelector('.ni-ref');
      }
    }
    return null;
  }

  // Indexes every correspondence node in `viz` by id:
  // { id, el (highlight target), pane, corresp:[ids] }.
  function collectNodes(viz) {
    var byId = {};
    function add(id, el, corresp) {
      if (!id || byId[id]) return;
      byId[id] = {
        id: id, el: el, pane: el.closest('.viz-pane'),
        corresp: corresp ? corresp.split(/\s+/).filter(Boolean) : []
      };
    }
    var boxes = viz.querySelectorAll('.rscan[data-node-id]');
    for (var i = 0; i < boxes.length; i++) {
      add(boxes[i].getAttribute('data-node-id'), boxes[i],
          boxes[i].getAttribute('data-corresp'));
    }
    var markers = viz.querySelectorAll('.ni-ref[data-node-id]');
    for (var j = 0; j < markers.length; j++) {
      var rect = markers[j].closest('.rect');
      if (rect) {
        add(markers[j].getAttribute('data-node-id'), rect,
            markers[j].getAttribute('data-corresp'));
      }
    }
    return byId;
  }

  // Undirected adjacency over the `data-corresp` edges.
  function buildAdj(byId) {
    var adj = {};
    function link(a, b) { (adj[a] = adj[a] || []).push(b); }
    Object.keys(byId).forEach(function (id) {
      byId[id].corresp.forEach(function (c) { link(id, c); link(c, id); });
    });
    return adj;
  }

  // Ids reachable from startId (excluding startId).
  function reachableIds(adj, startId) {
    var seen = {}, stack = [startId], out = [];
    seen[startId] = true;
    while (stack.length) {
      var ns = adj[stack.pop()] || [];
      for (var i = 0; i < ns.length; i++) {
        if (!seen[ns[i]]) {
          seen[ns[i]] = true; out.push(ns[i]); stack.push(ns[i]);
        }
      }
    }
    return out;
  }

  function clearHighlights(viz) {
    var marked = viz.querySelectorAll('.viz-selected, .viz-corresp');
    for (var i = 0; i < marked.length; i++) {
      marked[i].classList.remove('viz-selected', 'viz-corresp');
    }
  }

  // Marks `clickedEl` as the primary selection and every node corresponding to
  // `startId` in the *other* panes as secondary.  Nodes reached back in the
  // clicked node's own pane are the "reflective" (tertiary) set and are left
  // un-highlighted for now (see the visualizer doc).
  function applyCorrespondence(viz, clickedEl, startId) {
    clearHighlights(viz);
    clickedEl.classList.add('viz-selected');
    if (startId == null) return;
    var byId = collectNodes(viz);
    var primaryPane = byId[startId] ? byId[startId].pane :
        (clickedEl.closest ? clickedEl.closest('.viz-pane') : null);
    reachableIds(buildAdj(byId), startId).forEach(function (id) {
      var rec = byId[id];
      if (!rec || rec.el === clickedEl) return;
      if (rec.pane && rec.pane === primaryPane) return;  // reflective: defer
      rec.el.classList.add('viz-corresp');
    });
  }

  function handleNodeClick(pane, e) {
    var viz = pane.closest('.viz');
    if (!viz) return;
    // Resolved AST / SQLBuilder box: id is on the `.rscan` itself.  (Details for
    // these panes are TBD.)
    var rscan = e.target.closest('.rscan');
    if (rscan && pane.contains(rscan)) {
      applyCorrespondence(viz, rscan, rscan.getAttribute('data-node-id'));
      return;
    }
    // Input box: show details, and drive correspondence from the innermost
    // enclosing `.rect` that carries a `.ni-ref` marker.
    var rect = e.target.closest('.rect');
    if (!rect || !pane.contains(rect)) return;
    var items = collectNodeInfo(rect, pane);
    if (items.length > 0) renderDetails(viz, items);
    var idRect = rect, id = null;
    while (idRect && pane.contains(idRect)) {
      if (idRect.classList && idRect.classList.contains('rect')) {
        var m = markerOfRect(idRect);
        if (m) { id = m.getAttribute('data-node-id'); break; }
      }
      idRect = idRect.parentElement;
    }
    applyCorrespondence(viz, (id != null ? idRect : rect), id);
  }

  // --- Window resize: columns scale proportionally via flex automatically.
  // The details box shrinks proportionally when the window shrinks, but never
  // grows beyond ~10 lines purely from window growth (manual drag may exceed
  // that, up to half the viewport). ---
  window.addEventListener('resize', function () {
    var viz = vizBlocks()[0];
    if (!viz) return;
    var infoEl = viz.querySelector('.viz-info');
    if (!infoEl || state.infoH == null) { state.lastVH = window.innerHeight; return; }
    var newVH = window.innerHeight;
    var ratio = newVH / (state.lastVH || newVH);
    var scaled = state.infoH * ratio;
    if (ratio > 1) {
      // Growing: do not let resize push it above 10 lines (or its current
      // height if the user already dragged it larger).
      var tenLines = 10 * lineHeightPx(infoEl) + 12;
      scaled = Math.min(scaled, Math.max(state.infoH, tenLines));
    }
    state.infoH = clampInfo(scaled, infoEl);
    state.lastVH = newVH;
    applyState();
  });

  // Activate single-column layout and initialize when a `.viz` is present.
  if (document.querySelector('.viz')) {
    document.documentElement.setAttribute('data-visualize', '1');
    applyState();
  }
})();
