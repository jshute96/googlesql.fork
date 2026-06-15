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
