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

  // Linked state shared by all `.viz` blocks.
  var state = {
    hidden: {},                                  // col key -> true
    weights: {input: 1, ast: 1, sqlbuilder: 1},  // flex-grow per column
    infoH: null,                                 // details-box height in px
    astView: 'text',                             // Resolved AST: 'text'|'graph'
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
      var infoEl = viz.querySelector('.viz-info');

      // Column visibility + widths.  A hidden column collapses in place to a
      // narrow strip rather than disappearing.
      COLS.forEach(function (k) {
        var col = viz.querySelector('.viz-col[data-col="' + k + '"]');
        if (!col) return;
        col.classList.toggle('collapsed', !!state.hidden[k]);
        col.style.setProperty('--w', state.weights[k]);
      });

      // A divider is live only between two DOM-adjacent expanded columns; it
      // stays in its natural slot (columns no longer reorder when collapsed).
      var pairs = [['input', 'ast'], ['ast', 'sqlbuilder']];
      Array.prototype.slice.call(viz.querySelectorAll('.viz-divider'))
          .forEach(function (d, i) {
            var pair = pairs[i];
            var live = pair && !state.hidden[pair[0]] && !state.hidden[pair[1]];
            d.classList.toggle('hidden', !live);
            if (live) { d.dataset.left = pair[0]; d.dataset.right = pair[1]; }
          });

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
    // Graph nodes are sized from the live DOM, so a graph built while the AST
    // column was hidden measured zero; rebuild now that the pane is visible.
    if (key === 'ast' && state.astView !== 'text') {
      vizBlocks().forEach(buildGraph);
    }
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

  // --- Hide / show (collapse / expand) clicks. ---
  document.addEventListener('click', function (e) {
    var hide = e.target.closest && e.target.closest('.viz-hide');
    if (hide) {
      var col = hide.closest('.viz-col');
      if (col) hideColumn(col.dataset.col);
      e.stopPropagation();
      return;
    }
    var show = e.target.closest && e.target.closest('.viz-show');
    if (show) {
      var scol = show.closest('.viz-col');
      if (scol) showColumn(scol.dataset.col);
      e.stopPropagation();
      return;
    }
    // Info-box ancestor heading click -> move selection to that node.
    var heading = e.target.closest && e.target.closest('.np-heading[data-np-idx]');
    if (heading) {
      var hviz = heading.closest('.viz');
      if (hviz) {
        selectDetailNode(hviz, parseInt(heading.getAttribute('data-np-idx'), 10));
        e.stopPropagation();
        return;
      }
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
          var refEl = info.querySelector('.ni-ref');
          items.push({
            title: titleEl ? titleEl.innerHTML : '',
            body: bodyEl ? bodyEl.innerHTML : '',
            el: el,
            id: refEl ? refEl.getAttribute('data-node-id') : null
          });
        }
      }
      el = el.parentElement;
    }
    return items;
  }

  // The detail items (with their `.el` references) backing the headings most
  // recently rendered into a viz's info box, so an ancestor heading click can
  // re-select that node.  Keyed by viz element.
  var lastDetailsByViz = new WeakMap();

  function renderDetails(viz, items) {
    var infoEl = viz ? viz.querySelector('.viz-info') : null;
    if (!infoEl) return;
    if (!items || items.length === 0) return;
    lastDetailsByViz.set(viz, items);
    // Heading stack (innermost first, outer ones prefixed "in") + body of the
    // selected (innermost) node.  Each heading re-selects its node when clicked
    // (the ancestors let you walk the selection outward).
    var html = '<div class="np-body-stack">';
    items.forEach(function (item, i) {
      var prefix = i === 0 ? '' : '<span class="np-in">in </span>';
      var clickable = item.el ? ' np-clickable" data-np-idx="' + i + '"' : '"';
      html += '<div class="np-heading' + (i === 0 ? ' selected' : '') +
          clickable + '>' +
          prefix + '<span class="np-title">' + item.title + '</span></div>';
    });
    html += '</div><div class="np-hr"></div><div class="np-body">' +
        items[0].body + '</div>';
    infoEl.innerHTML = html;
  }

  // Activates one heading in the (already-rendered) details stack: bolds it,
  // swaps the body below the divider to that item's text, and moves the
  // cross-pane highlight to its node -- without rebuilding the stack, so every
  // ancestor stays in the list and you can click back and forth between them.
  function selectDetailNode(viz, idx) {
    var items = lastDetailsByViz.get(viz);
    if (!items || !items[idx]) return;
    var infoEl = viz.querySelector('.viz-info');
    if (!infoEl) return;
    var headings = infoEl.querySelectorAll('.np-heading');
    for (var i = 0; i < headings.length; i++) {
      headings[i].classList.toggle('selected', i === idx);
    }
    var bodyEl = infoEl.querySelector('.np-body');
    if (bodyEl) bodyEl.innerHTML = items[idx].body || '';
    var it = items[idx];
    if (it.el) {
      var startId = it.id != null ? it.id
          : (it.el.getAttribute ? it.el.getAttribute('data-node-id') : null);
      applyCorrespondence(viz, it.el, startId);
    }
  }

  // A SQLBuilder `.rscan` box's own leading HTML: its operator text up to (not
  // including) any nested `.rscan-sub` subquery block.  The box's innerHTML is
  // already HTML-escaped, so the slice is safe to re-insert.
  function sqlBoxOwnHtml(box) {
    var h = box.innerHTML;
    // Match the opening of a nested block whether its class is just "rscan-sub"
    // (a subquery) or "rscan-sub rscan-subpipeline" (a |> FORK branch); stopping
    // before it keeps only this operator's own leading text (e.g. "|> FORK (")
    // rather than the whole multi-line body.
    var idx = h.indexOf('<div class="rscan-sub');
    return idx >= 0 ? h.slice(0, idx) : h;
  }

  // Builds the details stack for a SQLBuilder pane box: the chain of enclosing
  // pipe-operator boxes (a nested subquery operator plus the operators that
  // contain it), innermost first.  Unlike the Resolved AST these boxes carry no
  // NameList -- the title is the operator's own keyword line and the body is its
  // own SQL text (excluding nested subquery operators, which are their own
  // items).  Mirrors collectScanHierarchy / collectNodeInfo for this pane.
  function collectSqlBuilderHierarchy(box, root) {
    var items = [], el = box;
    while (el && root.contains(el)) {
      if (el.classList && el.classList.contains('rscan')) {
        var lead = sqlBoxOwnHtml(el);
        // Title is the operator's first line, without a trailing "(" left by a
        // following nested block (so "|> FORK (" reads as "|> FORK").
        var first = lead.split('\n')[0].trim().replace(/\s*\(+\s*$/, '');
        items.push({
          title: first || '(pipe operator)', body: lead, el: el,
          id: el.getAttribute('data-node-id')
        });
      } else if (el.classList && el.classList.contains('rscan-subpipeline')) {
        // A `|> FORK` (or similar) branch: a subpipeline step in the hierarchy,
        // between the operators inside it and the operator that contains them.
        items.push({ title: 'Subpipeline', body: '', el: el, id: null });
      } else if (el.classList && el.classList.contains('rscan-stmt')) {
        items.push({ title: 'SQLBuilder SQL', body: '', el: el, id: null });
      }
      el = el.parentElement;
    }
    return items;
  }

  // First / all direct children of `el` with class `cls`.
  function directChild(el, cls) {
    for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
      if (c.classList && c.classList.contains(cls)) return c;
    }
    return null;
  }
  function directChildren(el, cls) {
    var out = [];
    for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
      if (c.classList && c.classList.contains(cls)) out.push(c);
    }
    return out;
  }

  // Builds the details stack for a Resolved AST `.rscan` / `.rscan-stmt` box:
  // the chain of enclosing scan / query boxes (title from each box's own
  // `.rscan-head`), innermost first, each with its own `.rscan-field`s as its
  // body and its `data-node-id` for highlighting.  Mirrors collectNodeInfo for
  // the input pane.  (Every level carries a body so clicking any heading can
  // show that level's details without rebuilding the stack.)
  function collectScanHierarchy(box, root) {
    var items = [], el = box;
    while (el && root.contains(el)) {
      if (el.classList && (el.classList.contains('rscan') ||
                           el.classList.contains('rscan-stmt'))) {
        var head = directChild(el, 'rscan-head');
        var body = directChildren(el, 'rscan-field').map(function (f) {
          return f.innerHTML;
        }).join('<br>');
        items.push({
          title: head ? head.innerHTML : '', body: body, el: el,
          id: el.getAttribute ? el.getAttribute('data-node-id') : null
        });
      }
      el = el.parentElement;
    }
    return items;
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

  function isVisible(el) {
    return !!(el && (el.offsetWidth || el.offsetHeight ||
                     el.getClientRects().length));
  }

  // Indexes every correspondence node in `viz` by id:
  // { id, el (highlight target), pane, corresp:[ids] }.  The same id can have
  // two elements when the Resolved AST column is in graph mode (the hidden text
  // box and the visible graph node); we highlight the visible one but keep the
  // `data-corresp` edges from whichever element carries them (the text box).
  function collectNodes(viz) {
    var byId = {};
    function add(id, el, corresp) {
      if (!id) return;
      var edges = corresp ? corresp.split(/\s+/).filter(Boolean) : [];
      var rec = byId[id];
      if (rec) {
        if (!isVisible(rec.el) && isVisible(el)) {
          rec.el = el;
          rec.pane = el.closest('.viz-pane');
        }
        if (rec.corresp.length === 0 && edges.length > 0) rec.corresp = edges;
        return;
      }
      byId[id] = {id: id, el: el, pane: el.closest('.viz-pane'),
                  corresp: edges};
    }
    var boxes = viz.querySelectorAll('.rscan[data-node-id]');
    for (var i = 0; i < boxes.length; i++) {
      add(boxes[i].getAttribute('data-node-id'), boxes[i],
          boxes[i].getAttribute('data-corresp'));
    }
    var gnodes = viz.querySelectorAll('.viz-gnode[data-node-id]');
    for (var k = 0; k < gnodes.length; k++) {
      add(gnodes[k].getAttribute('data-node-id'), gnodes[k],
          gnodes[k].getAttribute('data-corresp'));
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

  function clearHighlights(viz) {
    var marked = viz.querySelectorAll('.viz-selected, .viz-corresp');
    for (var i = 0; i < marked.length; i++) {
      marked[i].classList.remove('viz-selected', 'viz-corresp');
    }
  }

  // Marks `clickedEl` as the primary selection and every node DIRECTLY
  // cross-referenced from `startId` in the *other* panes as secondary.  The walk
  // is one hop only: it deliberately does NOT chase transitive links.  A
  // transitive walk would merge a whole connected component -- e.g. every source
  // TableScan and JoinScan, linked through the shared source-block SQLBuilder
  // segments, plus all their input tables -- into one confusing "reflective"
  // blob.  One hop keeps each click's highlight to its direct correspondents.
  function applyCorrespondence(viz, clickedEl, startId) {
    clearHighlights(viz);
    clickedEl.classList.add('viz-selected');
    if (startId == null) return;
    var byId = collectNodes(viz);
    var primaryPane = byId[startId] ? byId[startId].pane :
        (clickedEl.closest ? clickedEl.closest('.viz-pane') : null);
    var corresp = [];                                   // highlighted records
    var adj = buildAdj(byId);
    var seen = {};
    (adj[startId] || []).forEach(function (id) {
      if (seen[id]) return;
      seen[id] = true;
      var rec = byId[id];
      if (!rec || rec.el === clickedEl) return;
      if (rec.pane && rec.pane === primaryPane) return;  // same pane: skip
      rec.el.classList.add('viz-corresp');
      corresp.push(rec);
    });
    scrollCorrespIntoView(clickedEl, primaryPane, corresp);
  }

  // Element's top offset within its scroll pane's content (px).
  function offsetInPane(el, pane) {
    return el.getBoundingClientRect().top - pane.getBoundingClientRect().top +
        pane.scrollTop;
  }

  // Scrolls each *other* pane so its topmost corresponding node aligns
  // vertically with the clicked node's position in its own pane's viewport.
  function scrollCorrespIntoView(clickedEl, clickedPane, corresp) {
    if (!clickedPane || !corresp.length) return;
    var anchor = clickedEl.getBoundingClientRect().top -
        clickedPane.getBoundingClientRect().top;       // viewport offset
    var topByPane = {};                                 // pane -> {pane, el, off}
    corresp.forEach(function (rec) {
      var pane = rec.pane;
      if (!pane || pane === clickedPane) return;
      var off = offsetInPane(rec.el, pane);
      // Key by pane identity by stashing a sequence id on the pane element.
      var cur = topByPane[pane.__vizKey || (pane.__vizKey = ++scrollPaneSeq)];
      if (!cur || off < cur.off) {
        topByPane[pane.__vizKey] = {pane: pane, el: rec.el, off: off};
      }
    });
    Object.keys(topByPane).forEach(function (k) {
      var entry = topByPane[k];
      var desired = Math.max(0, entry.off - anchor);
      try { entry.pane.scrollTo({top: desired, behavior: 'smooth'}); }
      catch (e) { entry.pane.scrollTop = desired; }
    });
  }
  var scrollPaneSeq = 0;

  function handleNodeClick(pane, e) {
    var viz = pane.closest('.viz');
    if (!viz) return;
    // Graph node (operator or query): show details from the corresponding text
    // `.rscan` box and drive cross-pane correspondence highlighting.
    var gnode = e.target.closest('.viz-gnode');
    if (gnode && pane.contains(gnode)) {
      var gid = gnode.getAttribute('data-node-id');
      // Details come from the corresponding (hidden) text `.rscan` box.  An
      // operator node shares the box's id; a query node (b<n>) has no box of
      // its own, so use its first aggregated operator (`data-corresp`).
      var detailId = gid;
      var box = viz.querySelector('.rscan[data-node-id="' + detailId + '"]');
      if (!box) {
        detailId = (gnode.getAttribute('data-corresp') || '').split(/\s+/)[0];
        box = detailId
            ? viz.querySelector('.rscan[data-node-id="' + detailId + '"]')
            : null;
      }
      if (box) {
        var gitems = collectScanHierarchy(box, viz);
        if (gitems.length > 0) renderDetails(viz, gitems);
      }
      applyCorrespondence(viz, gnode, gid);
      return;
    }
    // Resolved AST / SQLBuilder box: id is on the `.rscan` itself.  The
    // statement-level `.rscan-stmt` box is selectable too (e.g. the outer
    // "QueryStmt"); it has no cross-pane correspondence of its own, so clicking
    // it just highlights that box and shows its details.
    var rscan = e.target.closest('.rscan, .rscan-stmt');
    if (rscan && pane.contains(rscan)) {
      // Details: a Resolved AST box has its own head/fields and shows the scan
      // hierarchy; a SQLBuilder segment carries only text, so show the
      // SQLBuilder pane's own pipe-operator hierarchy (it has no NameList).
      var items;
      if (rscan.classList.contains('rscan') && !directChild(rscan, 'rscan-head')) {
        items = collectSqlBuilderHierarchy(rscan, viz);
      } else {
        items = collectScanHierarchy(rscan, viz);
      }
      if (items.length > 0) renderDetails(viz, items);
      applyCorrespondence(viz, rscan, rscan.getAttribute('data-node-id'));
      return;
    }
    // Input box: show details for the clicked node, and drive cross-pane
    // correspondence from the innermost enclosing `.rect` that carries a
    // `.ni-ref` marker (its producing scan).
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
    // The primary selection (red box) is the clicked node's *own* box -- the
    // innermost box whose details we show -- not the marker-bearing ancestor
    // scan box.  A non-scan node like a function call has its own `.rect` (and
    // info) but no marker, so its marker id resolves to the enclosing scan;
    // without this the red box would jump out to that enclosing operator.
    var primaryEl = (items.length > 0 && items[0].el) ? items[0].el : rect;
    applyCorrespondence(viz, primaryEl, id);
  }

  // --- Graph view (operator mode) -----------------------------------------
  // An initial, hermetic node-link rendering of the Resolved AST's QueryGraph
  // (embedded as JSON in `.viz-graph-data`).  Layout is a simple layered
  // placement: rows are the longest path from a source so data flows downward,
  // the pipe spine stays in one column, and each secondary input branches into
  // a fresh column to the right.  This is deliberately minimal; a
  // constraint-based layout (elkjs) and cross-view correspondence highlighting
  // are planned follow-ups.  Any error falls back to the text rendering.
  // Layout padding and the gaps between node columns/rows.  Node sizes are
  // measured from their (variable) content rather than fixed, so these are only
  // spacing constants plus minimums for empty nodes.
  var GPAD = 16, GCOL_GAP = 36, GROW_GAP = 26, GNODE_MINW = 60, GNODE_MINH = 24;
  var SVGNS = 'http://www.w3.org/2000/svg';

  function parseGraph(viz) {
    var el = viz.querySelector('.viz-graph-data[data-graph="ast"]');
    if (!el) return null;
    try { return JSON.parse(el.textContent); } catch (e) { return null; }
  }

  // Computes { colOf, rowOf, cols, rows } for the operator nodes.
  function layoutGraph(g) {
    var edgesTo = {};        // consumer id -> [{from, kind}]
    var isProducer = {};     // id -> appears as some edge.from
    g.nodes.forEach(function (n) { edgesTo[n.id] = []; });
    g.edges.forEach(function (e) {
      (edgesTo[e.to] = edgesTo[e.to] || []).push({from: e.from, kind: e.kind});
      isProducer[e.from] = true;
    });

    // Row = longest path (in edges) from a source.  Memoized DFS with a cycle
    // guard (the model is a DAG, but stay defensive).
    var rowOf = {}, inProg = {};
    function row(id) {
      if (rowOf[id] != null) return rowOf[id];
      if (inProg[id]) return 0;
      inProg[id] = true;
      var r = 0;
      (edgesTo[id] || []).forEach(function (p) {
        r = Math.max(r, row(p.from) + 1);
      });
      inProg[id] = false;
      return (rowOf[id] = r);
    }
    g.nodes.forEach(function (n) { row(n.id); });

    // Column: place each sink and recurse to its producers; a pipe producer
    // keeps the column, each secondary input opens a fresh column to the right.
    var colOf = {}, nextCol = 0, seen = {};
    function place(id, col) {
      if (seen[id]) return;
      seen[id] = true;
      colOf[id] = col;
      var prods = edgesTo[id] || [];
      prods.forEach(function (p) {
        if (p.kind === 'pipe') place(p.from, col);
      });
      prods.forEach(function (p) {
        if (p.kind !== 'pipe') place(p.from, ++nextCol);
      });
    }
    // Sinks (never a producer) first, then anything left disconnected.
    g.nodes.forEach(function (n) {
      if (!isProducer[n.id] && !seen[n.id]) place(n.id, nextCol++);
    });
    g.nodes.forEach(function (n) {
      if (!seen[n.id]) place(n.id, nextCol++);
    });

    var maxRow = 0, maxCol = 0;
    g.nodes.forEach(function (n) {
      maxRow = Math.max(maxRow, rowOf[n.id] || 0);
      maxCol = Math.max(maxCol, colOf[n.id] || 0);
    });
    return {colOf: colOf, rowOf: rowOf, cols: maxCol + 1, rows: maxRow + 1};
  }

  // Fills graph node `d` with the per-operator Resolved AST content cloned from
  // the hidden text `.rscan` box(es) for `memberIds` -- the same head + scalar
  // fields the text pane shows for each operator (nested child-scan boxes are
  // omitted: they are separate graph nodes joined by edges).  An operator node
  // has a single member (its own id); a collapsed query node stacks all the
  // operators it aggregates.  Falls back to the `kind` label if no box is found.
  function fillNodeContent(viz, d, memberIds, kind) {
    var any = false;
    memberIds.forEach(function (id) {
      var box = viz.querySelector('.rscan[data-node-id="' + id + '"]');
      if (!box) return;
      var part = document.createElement('div');
      part.className = 'viz-gnode-op';
      var head = directChild(box, 'rscan-head');
      if (head) part.appendChild(head.cloneNode(true));
      directChildren(box, 'rscan-field').forEach(function (f) {
        part.appendChild(f.cloneNode(true));
      });
      d.appendChild(part);
      any = true;
    });
    if (!any) d.textContent = kind;
  }

  // Renders graph `g` into `pane`.  `correspOf(id)` returns the ids this node
  // corresponds to (set as `data-corresp`); operator nodes share the text box's
  // id and inherit its edges, so they pass `null`, but query nodes (ids in the
  // `b<n>` space) list the operator ids they aggregate.
  //
  // Nodes are sized to their content, so layout runs in two passes: build and
  // append the nodes (assigned to a layered col/row), measure them, then size
  // each column to its widest node and each row to its tallest and position
  // accordingly.  Edges are drawn last from the measured geometry.
  function renderGraph(viz, pane, g, correspOf) {
    var lay = layoutGraph(g);
    var canvas = document.createElement('div');
    canvas.className = 'viz-graph';
    pane.appendChild(canvas);

    // Pass 1: create + append node elements (still unpositioned) so the browser
    // lays out their content and we can measure it.
    var els = {};                       // id -> element
    g.nodes.forEach(function (n) {
      if (lay.colOf[n.id] == null) return;
      var d = document.createElement('div');
      d.className = 'viz-gnode';
      d.setAttribute('data-node-id', n.id);
      var corresp = correspOf ? correspOf(n.id) : null;
      if (corresp && corresp.length) {
        d.setAttribute('data-corresp', corresp.join(' '));
      }
      fillNodeContent(viz, d, (corresp && corresp.length) ? corresp : [n.id],
                      n.kind);
      canvas.appendChild(d);
      els[n.id] = d;
    });

    // Pass 2: per-column max width and per-row max height from measurements.
    var colW = [], rowH = [];
    Object.keys(els).forEach(function (id) {
      var c = lay.colOf[id], r = lay.rowOf[id];
      colW[c] = Math.max(colW[c] || GNODE_MINW, els[id].offsetWidth);
      rowH[r] = Math.max(rowH[r] || GNODE_MINH, els[id].offsetHeight);
    });
    var colX = [], x = GPAD;
    for (var c = 0; c < lay.cols; c++) {
      colX[c] = x;
      x += (colW[c] || GNODE_MINW) + GCOL_GAP;
    }
    var rowY = [], y = GPAD;
    for (var r = 0; r < lay.rows; r++) {
      rowY[r] = y;
      y += (rowH[r] || GNODE_MINH) + GROW_GAP;
    }
    var W = x - GCOL_GAP + GPAD, H = y - GROW_GAP + GPAD;

    // Center of a column (all nodes in a column are centered in it, so a pipe
    // spine draws as a straight vertical line); top/bottom of a node's box.
    function midX(id) { return colX[lay.colOf[id]] + colW[lay.colOf[id]] / 2; }
    function nodeTop(id) { return rowY[lay.rowOf[id]]; }
    function nodeBot(id) { return nodeTop(id) + els[id].offsetHeight; }

    // Position each node, centered horizontally within its column.
    Object.keys(els).forEach(function (id) {
      var el = els[id], cw = colW[lay.colOf[id]];
      el.style.left = (colX[lay.colOf[id]] + (cw - el.offsetWidth) / 2) + 'px';
      el.style.top = nodeTop(id) + 'px';
    });

    canvas.style.width = W + 'px';
    canvas.style.height = H + 'px';

    // Edge overlay (behind the nodes), arrowheads pointing down.
    var svg = document.createElementNS(SVGNS, 'svg');
    svg.setAttribute('class', 'viz-graph-edges');
    svg.setAttribute('width', W);
    svg.setAttribute('height', H);
    var defs = document.createElementNS(SVGNS, 'defs');
    defs.innerHTML =
        '<marker id="viz-arrow" markerWidth="9" markerHeight="9" refX="6"' +
        ' refY="3" orient="auto" markerUnits="userSpaceOnUse">' +
        '<path d="M0,0 L6,3 L0,6 Z"></path></marker>';
    svg.appendChild(defs);
    g.edges.forEach(function (e) {
      if (lay.colOf[e.from] == null || lay.colOf[e.to] == null) return;
      var x1 = midX(e.from), y1 = nodeBot(e.from);  // producer bottom
      var x2 = midX(e.to), y2 = nodeTop(e.to);       // consumer top
      var path = document.createElementNS(SVGNS, 'path');
      path.setAttribute('d', 'M' + x1 + ',' + y1 + ' L' + x2 + ',' + y2);
      path.setAttribute('class', 'viz-edge viz-edge-' + e.kind);
      path.setAttribute('marker-end', 'url(#viz-arrow)');
      svg.appendChild(path);
    });
    canvas.insertBefore(svg, canvas.firstChild);
  }

  // Collapses the operator graph to a query graph: one node per container that
  // holds operators, edges between containers derived from the inter-container
  // operator edges (deduped), and `members[containerId] = [operator ids]` so a
  // query node can correspond to the scans it aggregates.
  function collapseToQueries(g) {
    var cont = {}, members = {};
    g.nodes.forEach(function (n) {
      cont[n.id] = n.container;
      (members[n.container] = members[n.container] || []).push(n.id);
    });
    var nodes = g.containers.filter(function (c) { return members[c.id]; })
        .map(function (c) {
          return {id: c.id, kind: c.kind, container: c.parent};
        });
    var seen = {}, edges = [];
    g.edges.forEach(function (e) {
      var a = cont[e.from], b = cont[e.to];
      if (!a || !b || a === b) return;
      var key = a + '>' + b + '>' + e.kind;
      if (seen[key]) return;
      seen[key] = true;
      edges.push({from: a, to: b, kind: e.kind, label: e.label});
    });
    return {graph: {nodes: nodes, edges: edges, containers: g.containers},
            members: members};
  }

  // (Re)builds the graph canvas in a `.viz` block's Resolved AST pane to match
  // `state.astView` (removing any stale canvas first).
  function buildGraph(viz) {
    var pane = viz.querySelector('.viz-col[data-col="ast"] .viz-pane');
    if (!pane) return;
    var stale = pane.querySelector('.viz-graph');
    if (stale) stale.parentNode.removeChild(stale);
    if (state.astView === 'text') return;
    var g = parseGraph(viz);
    if (!g || !g.nodes || !g.nodes.length) return;
    try {
      if (state.astView === 'query') {
        var collapsed = collapseToQueries(g);
        renderGraph(viz, pane, collapsed.graph, function (id) {
          return collapsed.members[id] || [];
        });
      } else {
        renderGraph(viz, pane, g, null);
      }
    } catch (err) { /* keep text view */ }
  }

  // Pushes the linked Resolved AST view mode onto every `.viz` block.
  function applyView() {
    vizBlocks().forEach(function (viz) {
      var astCol = viz.querySelector('.viz-col[data-col="ast"]');
      if (astCol) {
        astCol.classList.toggle('graph-mode', state.astView !== 'text');
      }
      var sel = viz.querySelector('.viz-view[data-col="ast"]');
      if (sel) sel.value = state.astView;
      buildGraph(viz);
    });
  }

  // View-mode selector (linked across all blocks).
  document.addEventListener('change', function (e) {
    var sel = e.target.closest ? e.target.closest('.viz-view') : null;
    if (!sel) return;
    var v = sel.value;
    state.astView = (v === 'operator' || v === 'query') ? v : 'text';
    applyView();
  });

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
    applyView();
  }
})();
