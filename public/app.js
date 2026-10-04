/* ============================================================================
 * app.js —— 聊天气室前端逻辑（纯原生 JS，无第三方依赖，无 ES module）
 *
 * 房间名（room）约定：
 *   - 世界聊天：固定为 "#world"。
 *   - 私聊房间名规则（唯一正确做法）：'dm:' + 两个数字 id 升序拼接，即
 *         'dm:' + Math.min(myId, friendId) + ':' + Math.max(myId, friendId)
 *     其中 myId 取自 GET /api/me（或 register/login 响应）的 "id" 字段；
 *     后端按数值排序归一化后查找，故双方只需算出同一房名。
 *
 * 安全：所有来自服务器的文本（消息正文、用户名、身份码等）在渲染前一律经
 *       escapeHtml() 转义，避免 XSS。
 * ========================================================================== */
(function () {
  'use strict';

  var POLL_INTERVAL = 1000; /* 轮询间隔（毫秒） */

  /* ------------------------------------------------------------------ 工具 */

  /** 转义 HTML，防止 XSS */
  function escapeHtml(value) {
    if (value === null || value === undefined) return '';
    return String(value).replace(/[&<>"']/g, function (ch) {
      switch (ch) {
        case '&': return '&amp;';
        case '<': return '&lt;';
        case '>': return '&gt;';
        case '"': return '&quot;';
        default:  return '&#39;';
      }
    });
  }

  /** 把普通对象编码为 application/x-www-form-urlencoded */
  function toForm(obj) {
    var parts = [];
    if (obj) {
      Object.keys(obj).forEach(function (key) {
        if (obj[key] === undefined || obj[key] === null) return;
        parts.push(encodeURIComponent(key) + '=' + encodeURIComponent(String(obj[key])));
      });
    }
    return parts.join('&');
  }

  /**
   * 统一 API 调用。
   * options: { method: 'GET'|'POST', body: Object }
   * 返回 Promise<data>，data 为解析后的 JSON（失败时含 error 字段）。
   */
  function api(path, options) {
    options = options || {};
    var init = {
      method: options.method || 'GET',
      credentials: 'same-origin', /* 让 Cookie（sid）生效 */
      headers: {}
    };
    if (options.body) {
      init.headers['Content-Type'] = 'application/x-www-form-urlencoded;charset=UTF-8';
      init.body = toForm(options.body);
    }
    return fetch(path, init).then(function (res) {
      return res.text().then(function (text) {
        var data = null;
        if (text) {
          try { data = JSON.parse(text); } catch (e) { data = null; }
        }
        if (!data || typeof data !== 'object') {
          data = { ok: false, error: '服务器响应异常（HTTP ' + res.status + '）' };
        }
        if (data.__status === undefined) data.__status = res.status;
        return data;
      });
    });
  }

  /* ------------------------------------------------------------------ 状态 */

  var state = {
    me: null,
    myId: 0,
    mode: 'guest',
    currentRoom: '#world',
    currentPeerId: null,
    after: 0,
    friends: [],
    requests: [],
    pollTimer: null,
    polling: false,
    toastTimer: null,
    /* 文件传输（WebRTC 信令与数据通道） */
    pc: {},                /* peerId -> RTCPeerConnection */
    transferSessions: {},  /* 'send:<peerId>' / 'recv:<peerId>' -> 会话 */
    pendingIce: {},        /* peerId -> 待 addIceCandidate 的候选队列 */
    transferPollTimer: null,
    transferPolling: false,
    /* 渲染签名：轮询每秒调用 render*，若每次都重建 DOM，按钮会在点击瞬间
     * 被替换成新节点导致点不中。数据没变时直接跳过重建。 */
    reqSig: null,
    friendSig: null
  };

  /* ------------------------------------------------------------------ DOM */

  function $(id) { return document.getElementById(id); }

  var el = {};

  function cacheDom() {
    [
      'screen-welcome', 'screen-app', 'modal', 'toast', 'lightbox',
      'btn-guest', 'btn-open-login', 'btn-open-register',
      'tab-login', 'tab-register', 'auth-form', 'in-username', 'in-password',
      'auth-error', 'auth-submit', 'modal-close',
      'room-name', 'conn-status', 'mode-tag', 'btn-logout', 'btn-sidebar-toggle', 'sidebar',
      'my-uid', 'my-handle', 'btn-edit-handle', 'handle-editor', 'in-handle',
      'btn-handle-save', 'btn-handle-cancel',
      'btn-world', 'req-badge', 'request-list', 'friend-list',
      'add-friend-card', 'in-add-friend', 'btn-add-friend',
      'messages', 'messages-empty', 'composer', 'in-message',
      'btn-attach', 'file-input'
    ].forEach(function (id) {
      el[id] = $(id);
    });
  }

  /* --------------------------------------------------------------- 提示 UI */

  function showToast(message, isError) {
    if (!el.toast) return;
    el.toast.textContent = message;
    el.toast.className = 'toast' + (isError ? ' toast-err' : '');
    if (state.toastTimer) clearTimeout(state.toastTimer);
    state.toastTimer = setTimeout(function () {
      el.toast.className = 'toast hidden';
    }, 2600);
  }

  function setConnected(ok) {
    if (!el['conn-status']) return;
    el['conn-status'].className = 'conn-status ' + (ok ? 'conn-ok' : 'conn-bad');
    el['conn-status'].title = ok ? '连接正常' : '连接中断，正在重试…';
  }

  function showAuthError(message) {
    if (!el['auth-error']) return;
    if (message) {
      el['auth-error'].textContent = message;
      el['auth-error'].classList.remove('hidden');
    } else {
      el['auth-error'].textContent = '';
      el['auth-error'].classList.add('hidden');
    }
  }

  /* --------------------------------------------------------------- 屏切换 */

  function showWelcome() {
    el['screen-welcome'].classList.remove('hidden');
    el['screen-app'].classList.add('hidden');
  }

  function showApp() {
    el['screen-welcome'].classList.add('hidden');
    el['screen-app'].classList.remove('hidden');
  }

  /* --------------------------------------------------------------- 弹窗 */

  var authMode = 'login';

  function openModal(mode) {
    setAuthMode(mode || 'login');
    showAuthError('');
    el.modal.classList.remove('hidden');
    if (el['in-username']) el['in-username'].focus();
  }

  function closeModal() {
    el.modal.classList.add('hidden');
    if (el['in-password']) el['in-password'].value = '';
  }

  function setAuthMode(mode) {
    authMode = mode === 'register' ? 'register' : 'login';
    var isLogin = authMode === 'login';
    el['tab-login'].classList.toggle('active', isLogin);
    el['tab-register'].classList.toggle('active', !isLogin);
    el['auth-submit'].textContent = isLogin ? '登录' : '注册';
    showAuthError('');
  }

  /* --------------------------------------------------------------- 身份 UI */

  function renderIdentity() {
    var me = state.me || {};
    el['my-uid'].textContent = me.uid || '—';
    var handle = me.handle || '';
    el['my-handle'].textContent = handle ? handle : '（未设置）';

    var isGuest = state.mode === 'guest';
    el['mode-tag'].textContent = isGuest ? '游客' : '登录';
    el['mode-tag'].className = 'mode-tag ' + (isGuest ? 'mode-guest' : 'mode-normal');

    /* 游客：隐藏修改身份码与添加好友 */
    el['btn-edit-handle'].classList.toggle('hidden', isGuest);
    el['add-friend-card'].classList.toggle('hidden', isGuest);
    el['btn-attach'].classList.toggle('hidden', isGuest);
    el['handle-editor'].classList.add('hidden');
  }

  /* --------------------------------------------------------------- 好友 UI */

  /* 渲染签名：只比对渲染结果真正依赖的字段，避免无关变化触发重建 */
  function signatureOf(mode, items, extra) {
    var parts = (items || []).map(function (it) {
      return String(it.id) + ':' + (it.handle || '') + ':' + (it.username || '');
    }).join(',');
    return mode + '|' + parts + '|' + (extra === undefined || extra === null ? '' : String(extra));
  }

  function renderRequests() {
    var sig = signatureOf(state.mode, state.requests);
    if (sig === state.reqSig) return;   /* 数据未变化：保留现有 DOM，按钮可正常点击 */
    state.reqSig = sig;

    var list = el['request-list'];
    list.textContent = '';

    if (state.mode === 'guest') {
      var hint = document.createElement('p');
      hint.className = 'empty-hint';
      hint.textContent = '登录后可使用好友功能';
      list.appendChild(hint);
      el['req-badge'].classList.add('hidden');
      return;
    }

    var reqs = state.requests || [];
    if (reqs.length === 0) {
      var p = document.createElement('p');
      p.className = 'empty-hint';
      p.textContent = '暂无待处理申请';
      list.appendChild(p);
      el['req-badge'].classList.add('hidden');
      return;
    }

    el['req-badge'].textContent = String(reqs.length);
    el['req-badge'].classList.remove('hidden');

    reqs.forEach(function (req) {
      var li = document.createElement('li');
      li.className = 'list-item';

      var main = document.createElement('div');
      main.className = 'item-main';
      var name = document.createElement('span');
      name.className = 'item-name';
      name.textContent = req.username || ('用户' + req.from_id); /* textContent 天然转义 */
      main.appendChild(name);
      if (req.handle) {
        var sub = document.createElement('span');
        sub.className = 'item-sub';
        sub.textContent = req.handle;
        main.appendChild(sub);
      }
      li.appendChild(main);

      var actions = document.createElement('div');
      actions.className = 'item-actions';

      var accept = document.createElement('button');
      accept.type = 'button';
      accept.className = 'btn btn-mini btn-primary';
      accept.textContent = '同意';
      accept.addEventListener('click', function () {
        respondRequest('accept', req.id, accept);
      });

      var reject = document.createElement('button');
      reject.type = 'button';
      reject.className = 'btn btn-mini';
      reject.textContent = '拒绝';
      reject.addEventListener('click', function () {
        respondRequest('reject', req.id, reject);
      });

      actions.appendChild(accept);
      actions.appendChild(reject);
      li.appendChild(actions);
      list.appendChild(li);
    });
  }

  function renderFriends() {
    /* 好友项带 active 高亮，所以签名里要带上当前私聊对象 */
    var sig = signatureOf(state.mode, state.friends, state.currentPeerId);
    if (sig === state.friendSig) return;
    state.friendSig = sig;

    var list = el['friend-list'];
    list.textContent = '';

    if (state.mode === 'guest') {
      var hint = document.createElement('p');
      hint.className = 'empty-hint';
      hint.textContent = '登录后可使用好友功能';
      list.appendChild(hint);
      return;
    }

    var friends = state.friends || [];
    if (friends.length === 0) {
      var p = document.createElement('p');
      p.className = 'empty-hint';
      p.textContent = '还没有好友，去添加一个吧';
      list.appendChild(p);
      return;
    }

    friends.forEach(function (friend) {
      var li = document.createElement('li');
      li.className = 'list-item clickable';
      li.dataset.friendId = String(friend.id);
      if (String(friend.id) === String(state.currentPeerId)) li.classList.add('active');

      var main = document.createElement('div');
      main.className = 'item-main';
      var name = document.createElement('span');
      name.className = 'item-name';
      name.textContent = friend.username || friend.handle || ('用户' + friend.id);
      main.appendChild(name);
      var sub = document.createElement('span');
      sub.className = 'item-sub';
      sub.textContent = friend.handle || ('#' + friend.id);
      main.appendChild(sub);
      li.appendChild(main);

      li.addEventListener('click', function () {
        openPrivateChat(friend);
      });
      list.appendChild(li);
    });
  }

  function loadFriends() {
    if (state.mode === 'guest') { renderFriends(); renderRequests(); return; }
    api('/api/friends').then(function (data) {
      if (data && data.ok) {
        state.friends = data.friends || [];
        state.requests = data.requests || [];
        renderFriends();
        renderRequests();
      }
    }).catch(function () { /* 静默失败（后端未就绪/离线） */ });
  }

  function respondRequest(action, id, button) {
    if (button) button.disabled = true;
    api('/api/friends/' + action, { method: 'POST', body: { id: id } })
      .then(function (data) {
        if (data && data.ok) {
          showToast(action === 'accept' ? '已同意好友申请' : '已拒绝好友申请', false);
          loadFriends();
        } else {
          if (button) button.disabled = false;
          showToast((data && data.error) || '操作失败', true);
        }
      })
      .catch(function () {
        if (button) button.disabled = false;
        showToast('网络错误，操作失败', true);
      });
  }

  function addFriend() {
    var input = el['in-add-friend'];
    var handle = (input.value || '').trim();
    if (!handle) { showToast('请输入对方身份码', true); return; }
    api('/api/friends/add', { method: 'POST', body: { handle: handle } })
      .then(function (data) {
        if (data && data.ok) {
          input.value = '';
          showToast(data.message || '好友申请已发送', false);
          loadFriends();
        } else {
          showToast((data && data.error) || '添加失败', true);
        }
      })
      .catch(function () { showToast('网络错误，添加失败', true); });
  }

  function saveHandle() {
    var value = (el['in-handle'].value || '').trim();
    if (!value) { showToast('请输入新身份码', true); return; }
    api('/api/handle', { method: 'POST', body: { handle: value } })
      .then(function (data) {
        if (data && data.ok) {
          state.me.handle = data.handle || value;
          renderIdentity();
          showToast('身份码已更新', false);
        } else {
          showToast((data && data.error) || '修改失败', true);
        }
      })
      .catch(function () { showToast('网络错误，修改失败', true); });
  }

  /* --------------------------------------------------------------- 房间 */

  /**
   * 计算私聊房间名：'dm:' + min(myId, friendId) + ':' + max(myId, friendId)
   * （后端按数值排序归一化，双方算出同一房名即可）
   */
  function dmRoom(friendId) {
    /* 私聊房间名规则：'dm:' + Math.min(myId, friendId) + ':' + Math.max(myId, friendId) */
    var myId = Number(state.myId ?? 0);
    var fid = Number(friendId) || 0;
    return 'dm:' + Math.min(myId, fid) + ':' + Math.max(myId, fid);
  }

  function setRoom(room, displayName, peerId) {
    state.currentRoom = room;
    state.currentPeerId = peerId === undefined ? null : peerId;
    state.after = 0;
    el['room-name'].textContent = displayName;
    el.messages.textContent = '';
    el['messages-empty'].classList.remove('hidden');
    if (el['btn-world']) el['btn-world'].classList.toggle('active', room === '#world');
    renderFriends(); /* 刷新好友高亮 */
    closeSidebarOnNarrow();
    if (state.me) pollNow(); /* 立刻拉取该房间历史消息 */
  }

  function openPrivateChat(friend) {
    var name = friend.username || friend.handle || ('用户' + friend.id);
    setRoom(dmRoom(friend.id), '与 ' + name + ' 私聊', friend.id);
  }

  function goWorld() {
    setRoom('#world', '世界聊天', null);
  }

  /* --------------------------------------------------------------- 消息 UI */

  function isOwnMessage(msg) {
    if (state.myId > 0 && Number(msg.sender_id) === state.myId) return true;
    var me = state.me || {};
    if (me.username && msg.sender === me.username) return true;
    if (me.uid && msg.sender === me.uid) return true;
    return false;
  }

  function renderMessage(msg) {
    /* 用自己发送的消息的 sender_id 推断并补全 myId */
    if (state.myId <= 0 && msg.sender_id) {
      var me = state.me || {};
      if (me.username && msg.sender === me.username) {
        state.myId = Number(msg.sender_id) || 0;
      }
    }

    var wrap = document.createElement('div');
    wrap.className = 'msg ' + (isOwnMessage(msg) ? 'msg-own' : 'msg-other');

    var meta = document.createElement('div');
    meta.className = 'msg-meta';
    var sender = document.createElement('span');
    sender.className = 'msg-sender';
    sender.textContent = isOwnMessage(msg) ? '我' : (msg.sender || '未知');
    var time = document.createElement('span');
    time.className = 'msg-time';
    time.textContent = msg.ts || '';
    meta.appendChild(sender);
    meta.appendChild(time);

    var bubble = document.createElement('div');
    bubble.className = 'msg-bubble';
    /* 经 escapeHtml 转义后再渲染，换行转为 <br>，防止 XSS */
    var body = msg.body === undefined || msg.body === null ? '' : String(msg.body);
    bubble.innerHTML = escapeHtml(body).replace(/\r\n|\r|\n/g, '<br>');

    wrap.appendChild(meta);
    wrap.appendChild(bubble);
    return wrap;
  }

  function appendMessages(messages) {
    if (!messages || !messages.length) return;
    var nearBottom = true;
    var box = el.messages;
    if (box) {
      nearBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 80;
    }
    var maxId = state.after;
    messages.forEach(function (msg) {
      if (typeof msg.id === 'number' && msg.id > maxId) maxId = msg.id;
      if (box) box.appendChild(renderMessage(msg));
    });
    state.after = maxId;
    el['messages-empty'].classList.add('hidden');
    if (nearBottom && box) box.scrollTop = box.scrollHeight;
  }

  function sendMessage() {
    var input = el['in-message'];
    var body = input.value;
    if (!body || !body.trim()) return;
    var room = state.currentRoom;
    input.value = '';
    api('/api/messages', { method: 'POST', body: { room: room, body: body } })
      .then(function (data) {
        if (data && data.ok) {
          pollNow(); /* 立即拉取，尽快显示自己发的消息 */
        } else {
          input.value = body; /* 发送失败，内容回填 */
          showToast((data && data.error) || '发送失败', true);
        }
      })
      .catch(function () {
        input.value = body;
        showToast('网络错误，发送失败', true);
      });
  }

  /* --------------------------------------------------------------- 文件传输 */

  function fmtSize(bytes) {
    if (bytes === null || bytes === undefined || isNaN(bytes)) return '';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
  }

  function mimeOfName(name) {
    var ext = String(name || '').split('.').pop().toLowerCase();
    var map = {
      png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
      webp: 'image/webp', bmp: 'image/bmp', svg: 'image/svg+xml', ico: 'image/x-icon',
      mp4: 'video/mp4', webm: 'video/webm', mov: 'video/quicktime',
      mkv: 'video/x-matroska', avi: 'video/x-msvideo',
      txt: 'text/plain', pdf: 'application/pdf', zip: 'application/zip'
    };
    return map[ext] || 'application/octet-stream';
  }

  function statusText(s) {
    return {
      waiting: '等待连接…',
      sending: '发送中…',
      receiving: '接收中…',
      sent: '已发送',
      done: '已完成',
      failed: '传输失败'
    }[s] || s || '';
  }

  function scrollMessagesBottom() {
    if (el.messages) el.messages.scrollTop = el.messages.scrollHeight;
  }

  function hexOf(buf) {
    var arr = Array.prototype.slice.call(new Uint8Array(buf));
    var s = '';
    for (var i = 0; i < arr.length; i++) {
      s += ('0' + arr[i].toString(16)).slice(-2);
    }
    return s;
  }

  function sha256Hex(file) {
    if (!window.crypto || !window.crypto.subtle) return Promise.resolve(null);
    return window.crypto.subtle.digest('SHA-256', file)
      .then(hexOf)
      .catch(function () { return null; });
  }

  function imageThumb(file) {
    return new Promise(function (resolve) {
      var url = URL.createObjectURL(file);
      var img = new Image();
      img.onload = function () {
        try {
          var scale = Math.min(1, 320 / Math.max(img.width, 1));
          var w = Math.max(1, Math.round(img.width * scale));
          var h = Math.max(1, Math.round(img.height * scale));
          var c = document.createElement('canvas');
          c.width = w; c.height = h;
          c.getContext('2d').drawImage(img, 0, 0, w, h);
          resolve(c.toDataURL('image/jpeg', 0.7));
        } catch (e) { resolve(null); }
        URL.revokeObjectURL(url);
      };
      img.onerror = function () { URL.revokeObjectURL(url); resolve(null); };
      img.src = url;
    });
  }

  function videoThumb(file) {
    return new Promise(function (resolve) {
      var url = URL.createObjectURL(file);
      var v = document.createElement('video');
      var settled = false;
      function finish(result) {
        if (settled) return;
        settled = true;
        try { URL.revokeObjectURL(url); } catch (e) {}
        resolve(result);
      }
      v.muted = true;
      v.preload = 'metadata';
      v.onloadedmetadata = function () {
        try { v.currentTime = Math.min(0.5, (v.duration || 0) / 2); }
        catch (e) { finish(null); }
      };
      v.onseeked = function () {
        try {
          var w = 320;
          var h = Math.max(1, Math.round(w * v.videoHeight / Math.max(v.videoWidth, 1)));
          var c = document.createElement('canvas');
          c.width = w; c.height = h;
          c.getContext('2d').drawImage(v, 0, 0, w, h);
          finish(c.toDataURL('image/jpeg', 0.7));
        } catch (e) { finish(null); }
      };
      v.onerror = function () { finish(null); };
      setTimeout(function () { finish(null); }, 4000); /* 兜底，避免一直 pending */
      v.src = url;
    });
  }

  function makeThumbnail(file) {
    var mime = file.type || mimeOfName(file.name);
    if (mime.indexOf('image/') === 0) return imageThumb(file);
    if (mime.indexOf('video/') === 0) return videoThumb(file);
    return Promise.resolve(null);
  }

  function prepareMeta(file) {
    var mime = file.type || mimeOfName(file.name);
    return Promise.all([sha256Hex(file), makeThumbnail(file)]).then(function (res) {
      return { name: file.name, size: file.size, mime: mime, sha256: res[0], thumb: res[1] || null };
    });
  }

  function friendName(id) {
    var found = null;
    (state.friends || []).forEach(function (f) {
      if (String(f.id) === String(id)) found = f;
    });
    return found ? (found.username || found.handle || ('用户' + found.id)) : ('好友' + id);
  }

  function createPeerConnection(peerId) {
    var pc = new RTCPeerConnection({ iceServers: [{ urls: 'stun:stun.l.google.com:19302' }] });
    state.pc[String(peerId)] = pc;
    pc.onicecandidate = function (e) {
      if (e.candidate) postSignal(peerId, 'ice', JSON.stringify(e.candidate));
    };
    return pc;
  }

  function postSignal(to, kind, payload) {
    return api('/api/transfer/send', {
      method: 'POST',
      body: { to: String(to), kind: kind, payload: payload }
    });
  }

  function appendFileCard(opts) {
    var wrap = document.createElement('div');
    wrap.className = 'msg ' + (opts.own ? 'msg-own' : 'msg-other');

    var meta = document.createElement('div');
    meta.className = 'msg-meta';
    var sender = document.createElement('span');
    sender.className = 'msg-sender';
    sender.textContent = opts.own ? '我' : (opts.sender || '文件');
    meta.appendChild(sender);
    wrap.appendChild(meta);

    var card = document.createElement('div');
    card.className = 'file-card';

    var preview = document.createElement('div');
    preview.className = 'file-preview';
    if (opts.thumb) {
      var thumb = document.createElement('img');
      thumb.className = 'file-thumb';
      thumb.src = opts.thumb;
      thumb.alt = opts.name || '';
      preview.appendChild(thumb);
    } else {
      var ph = document.createElement('div');
      ph.className = 'file-thumb file-thumb-placeholder';
      ph.textContent = /^video\//.test(opts.mime || '')
        ? '🎬' : (/^image\//.test(opts.mime || '') ? '🖼️' : '📄');
      preview.appendChild(ph);
    }

    var overlay = document.createElement('div');
    overlay.className = 'file-overlay';
    overlay.textContent = statusText(opts.status);
    preview.appendChild(overlay);

    var info = document.createElement('div');
    info.className = 'file-info';
    var nameEl = document.createElement('div');
    nameEl.className = 'file-name';
    nameEl.textContent = opts.name || '未命名文件';
    var subEl = document.createElement('div');
    subEl.className = 'file-sub';
    subEl.textContent = fmtSize(opts.size) + ' · ' + statusText(opts.status);
    var barWrap = document.createElement('div');
    barWrap.className = 'file-progress';
    var bar = document.createElement('div');
    bar.className = 'file-progress-bar';
    barWrap.appendChild(bar);
    var actions = document.createElement('div');
    actions.className = 'file-actions';

    info.appendChild(nameEl);
    info.appendChild(subEl);
    info.appendChild(barWrap);
    info.appendChild(actions);
    card.appendChild(preview);
    card.appendChild(info);
    wrap.appendChild(card);

    el.messages.appendChild(wrap);
    el['messages-empty'].classList.add('hidden');
    scrollMessagesBottom();

    return {
      wrap: wrap,
      preview: preview,
      overlay: overlay,
      sub: subEl,
      bar: bar,
      barWrap: barWrap,
      actions: actions,
      setProgress: function (done, total) {
        var pct = total > 0 ? Math.min(100, Math.round(done / total * 100)) : 100;
        bar.style.width = pct + '%';
        subEl.textContent = fmtSize(opts.size) + ' · ' + fmtSize(done) + ' (' + pct + '%)';
        overlay.textContent = opts.own ? '发送中 ' + pct + '%' : '接收中 ' + pct + '%';
      },
      setStatus: function (text, isErr) {
        subEl.textContent = fmtSize(opts.size) + ' · ' + text;
        overlay.textContent = text;
        if (isErr) card.classList.add('file-card-err');
      },
      setDone: function (objectUrl, mime) {
        overlay.classList.add('hidden');
        barWrap.classList.add('hidden');
        subEl.textContent = fmtSize(opts.size) + ' · 已完成';
        if (objectUrl && /^image\//.test(mime || '')) {
          preview.textContent = '';
          var img = document.createElement('img');
          img.className = 'file-thumb file-full';
          img.src = objectUrl;
          img.alt = opts.name || '';
          img.title = '点击查看大图';
          img.addEventListener('click', function () { openLightbox(objectUrl, 'image'); });
          preview.appendChild(img);
        } else if (objectUrl && /^video\//.test(mime || '')) {
          preview.textContent = '';
          var v = document.createElement('video');
          v.className = 'file-thumb file-full';
          v.controls = true;
          v.src = objectUrl;
          preview.appendChild(v);
        }
        var a = document.createElement('a');
        a.className = 'btn btn-mini';
        a.textContent = '下载';
        a.href = objectUrl || '#';
        a.download = opts.name || 'download';
        actions.appendChild(a);
        if (objectUrl) {
          var openBtn = document.createElement('button');
          openBtn.type = 'button';
          openBtn.className = 'btn btn-mini';
          openBtn.textContent = '全屏';
          openBtn.addEventListener('click', function () {
            openLightbox(objectUrl, (mime || '').indexOf('video/') === 0 ? 'video' : 'image');
          });
          actions.appendChild(openBtn);
        }
      }
    };
  }

  function openLightbox(url, kind) {
    var box = el.lightbox;
    if (!box) return;
    box.textContent = '';
    var node;
    if (kind === 'video') {
      node = document.createElement('video');
      node.controls = true;
      node.autoplay = true;
    } else {
      node = document.createElement('img');
    }
    node.src = url;
    box.appendChild(node);
    box.classList.remove('hidden');
  }

  function sendFileData(channel, file, meta, card) {
    channel.send(JSON.stringify({
      type: 'meta', name: meta.name, size: meta.size,
      mime: meta.mime, sha256: meta.sha256, thumb: meta.thumb
    }));

    /* 16KiB：SCTP 数据通道规范保证的最小可发送消息大小。
     * 之前用 64KiB 会触碰浏览器 maxMessageSize 边界，导致 send() 抛异常、
     * 接收方拿到 0 字节（文件损坏 / 下载 0KB）。 */
    var CHUNK = 16 * 1024;
    var HIGH = 1024 * 1024;                 /* 高水位：缓冲超过就先暂停发送 */
    channel.bufferedAmountLowThreshold = 256 * 1024;
    var offset = 0;
    var finished = false;

    function fail() {
      if (finished) return;
      finished = true;
      try { channel.send(JSON.stringify({ type: 'abort' })); } catch (e) {}
      channel.onbufferedamountlow = null;
      if (card) card.setStatus('传输失败', true);
    }

    function pump() {
      if (finished) return;
      while (offset < file.size && channel.bufferedAmount < HIGH) {
        try {
          channel.send(file.slice(offset, offset + CHUNK));
        } catch (e) {
          fail();
          return;
        }
        offset += CHUNK;
        if (card) card.setProgress(offset, file.size);
      }
      if (offset >= file.size) {
        finished = true;
        try { channel.send(JSON.stringify({ type: 'done' })); } catch (e) {}
        channel.onbufferedamountlow = null;
        if (card) card.setStatus('已发送', false);
        return;
      }
      channel.onbufferedamountlow = pump;
    }

    pump();
  }

  function sendFile(file) {
    if (state.mode === 'guest') { showToast('游客不能发送文件', true); return; }
    var peerId = String(state.currentPeerId);
    if (!peerId) { showToast('请先选择一位好友进入私聊', true); return; }

    prepareMeta(file).then(function (meta) {
      var pc = createPeerConnection(peerId);
      var channel = pc.createDataChannel('file', { ordered: true });
      var card = appendFileCard({
        own: true, name: file.name, size: file.size,
        mime: meta.mime, thumb: meta.thumb, status: 'waiting'
      });
      state.transferSessions['send:' + peerId] = { dir: 'send', channel: channel, card: card };
      channel.onopen = function () { sendFileData(channel, file, meta, card); };
      channel.onerror = function () { card.setStatus('传输失败', true); };
      pc.createOffer()
        .then(function (offer) { return pc.setLocalDescription(offer); })
        .then(function () { return postSignal(peerId, 'offer', JSON.stringify(pc.localDescription)); })
        .catch(function () { card.setStatus('传输失败', true); });
    });
  }

  function setupReceiveChannel(peerId, channel) {
    channel.binaryType = 'arraybuffer';
    var session = { dir: 'recv', channel: channel, chunks: [], meta: null, received: 0, card: null };
    state.transferSessions['recv:' + peerId] = session;
    channel.onmessage = function (e) {
      if (typeof e.data === 'string') {
        var msg = null;
        try { msg = JSON.parse(e.data); } catch (err) { msg = {}; }
        if (!msg) msg = {};
        if (msg.type === 'meta') {
          session.meta = msg;
          session.card = appendFileCard({
            own: false, sender: friendName(peerId), name: msg.name, size: msg.size,
            mime: msg.mime, thumb: msg.thumb, status: 'receiving'
          });
        } else if (msg.type === 'done') {
          finishReceive(peerId, session);
        } else if (msg.type === 'abort') {
          if (session.card) session.card.setStatus('传输失败', true);
        }
      } else {
        var size = e.data ? (e.data.byteLength || e.data.size || 0) : 0;
        session.chunks.push(e.data);
        session.received += size;
        if (session.card && session.meta) session.card.setProgress(session.received, session.meta.size);
      }
    };
    channel.onerror = function () { if (session.card) session.card.setStatus('传输失败', true); };
  }

  function verifyHash(blob, expectHex) {
    if (!window.crypto || !window.crypto.subtle || !expectHex) return Promise.resolve(true);
    return window.crypto.subtle.digest('SHA-256', blob)
      .then(hexOf)
      .then(function (hex) { return hex === expectHex; })
      .catch(function () { return true; });
  }

  function finishReceive(peerId, session) {
    var meta = session.meta;
    var card = session.card;
    if (!meta) {
      if (card) card.setStatus('传输失败', true);
      return;
    }
    if (session.received === 0 || session.chunks.length === 0) {
      if (card) card.setStatus('传输失败', true);
      return;
    }
    var blob = new Blob(session.chunks, { type: meta.mime || 'application/octet-stream' });
    if (card) card.setStatus('校验中…', false);
    verifyHash(blob, meta.sha256).then(function (ok) {
      if (!ok) {
        if (card) card.setStatus('校验失败', true);
        showToast('文件校验失败', true);
        return;
      }
      var url = URL.createObjectURL(blob);
      if (card) card.setDone(url, meta.mime);
      showToast('文件接收完成', false);
    });
  }

  function flushIce(peerId, pc) {
    var q = state.pendingIce[peerId] || [];
    state.pendingIce[peerId] = [];
    q.forEach(function (c) { pc.addIceCandidate(c).catch(function () {}); });
  }

  function handleOffer(sig) {
    var peerId = String(sig.from);
    var pc = createPeerConnection(peerId);
    pc.ondatachannel = function (e) { setupReceiveChannel(peerId, e.channel); };
    try {
      pc.setRemoteDescription(JSON.parse(sig.payload)).then(function () {
        flushIce(peerId, pc);
        return pc.createAnswer();
      }).then(function (answer) {
        return pc.setLocalDescription(answer);
      }).then(function () {
        return postSignal(peerId, 'answer', JSON.stringify(pc.localDescription));
      }).catch(function () {});
    } catch (e) {}
  }

  function handleAnswer(sig) {
    var peerId = String(sig.from);
    var pc = state.pc[peerId];
    if (!pc) return;
    try {
      pc.setRemoteDescription(JSON.parse(sig.payload)).then(function () {
        flushIce(peerId, pc);
      }).catch(function () {});
    } catch (e) {}
  }

  function handleIce(sig) {
    var peerId = String(sig.from);
    var pc = state.pc[peerId];
    var cand = null;
    try { cand = JSON.parse(sig.payload); } catch (e) { return; }
    if (!pc) {
      /* offer 尚未到达（信令乱序）：先排队，等 handleOffer 建好连接后再补加 */
      (state.pendingIce[peerId] = state.pendingIce[peerId] || []).push(cand);
      return;
    }
    if (pc.remoteDescription && pc.remoteDescription.type) {
      pc.addIceCandidate(cand).catch(function () {});
    } else {
      (state.pendingIce[peerId] = state.pendingIce[peerId] || []).push(cand);
    }
  }

  function handleSignal(sig) {
    if (!sig || !sig.kind) return;
    if (sig.kind === 'offer') handleOffer(sig);
    else if (sig.kind === 'answer') handleAnswer(sig);
    else if (sig.kind === 'ice') handleIce(sig);
  }

  function transferPoll() {
    if (!state.me || state.mode === 'guest') { scheduleTransferPoll(); return; }
    if (state.transferPolling) { scheduleTransferPoll(); return; }
    state.transferPolling = true;
    api('/api/transfer/poll').then(function (data) {
      state.transferPolling = false;
      if (data && data.ok && Array.isArray(data.signals)) {
        data.signals.forEach(handleSignal);
      }
      scheduleTransferPoll();
    }).catch(function () {
      state.transferPolling = false;
      scheduleTransferPoll();
    });
  }

  function scheduleTransferPoll() {
    if (state.transferPollTimer) clearTimeout(state.transferPollTimer);
    state.transferPollTimer = setTimeout(transferPoll, 1000);
  }

  function startTransferPoll() {
    if (state.transferPollTimer) return;
    scheduleTransferPoll();
  }

  function stopTransferPoll() {
    if (state.transferPollTimer) { clearTimeout(state.transferPollTimer); state.transferPollTimer = null; }
    state.transferPolling = false;
  }

  /* --------------------------------------------------------------- 轮询 */

  function setConnFromResult(ok) {
    if (state.connectedLast !== ok) {
      state.connectedLast = ok;
      setConnected(ok);
    }
  }

  function schedulePoll(delay) {
    if (state.pollTimer) clearTimeout(state.pollTimer);
    state.pollTimer = setTimeout(poll, delay === undefined ? POLL_INTERVAL : delay);
  }

  function pollNow() {
    if (state.pollTimer) { clearTimeout(state.pollTimer); state.pollTimer = null; }
    poll();
  }

  function poll() {
    if (!state.me) return;
    if (state.polling) { schedulePoll(POLL_INTERVAL); return; }
    state.polling = true;

    /* 记下本次请求的房间：请求期间用户可能切换房间，
     * 旧房间的响应一旦写入，既会串台，又会把 after 抬到旧房间的 id 上，
     * 导致新房间里 id 更小的历史消息被永久过滤掉。 */
    var room = state.currentRoom;
    var url = '/api/poll?room=' + encodeURIComponent(room) +
              '&after=' + encodeURIComponent(String(state.after));

    api(url).then(function (data) {
      state.polling = false;

      if (room !== state.currentRoom) {
        schedulePoll(POLL_INTERVAL);   /* 已切房间：丢弃这个过期响应 */
        return;
      }

      if (data && data.ok) {
        setConnFromResult(true);
        if (Array.isArray(data.messages)) appendMessages(data.messages);
        if (Array.isArray(data.requests)) {
          state.requests = data.requests;
          renderRequests();
        }
        if (typeof data.last_id === 'number' && data.last_id > state.after) {
          state.after = data.last_id;
        }
      } else if (data && data.__status === 403) {
        /* 私聊已失效（对方删除好友 / 已非好友）：留在错误房间里轮询没有意义，退回世界聊天。
         * goWorld() 内部会重新发起轮询，这里不再额外 schedule。 */
        showToast((data.error || '私聊已失效') + '，已返回世界聊天', true);
        goWorld();
        return;
      } else {
        /* 其它业务失败静默重试 */
        setConnFromResult(false);
      }
      schedulePoll(POLL_INTERVAL);
    }).catch(function () {
      /* 网络错误静默重试 */
      state.polling = false;
      setConnFromResult(false);
      schedulePoll(POLL_INTERVAL);
    });
  }

  function stopPoll() {
    if (state.pollTimer) { clearTimeout(state.pollTimer); state.pollTimer = null; }
    state.polling = false;
  }

  /* --------------------------------------------------------------- 会话 */

  function enterApp(me) {
    state.me = me || {};
    state.mode = state.me.mode === 'guest' || state.me.is_guest ? 'guest' : 'normal';
    /* register / login / guest / me 的响应均带数字 id；缺失时用 0 兜底 */
    state.myId = Number(state.me.id ?? 0) || 0;

    showApp();
    closeModal();
    renderIdentity();
    renderFriends();
    renderRequests();

    goWorld();
    if (state.mode !== 'guest') loadFriends();
    if (state.mode !== 'guest') startTransferPoll();
    pollNow();
  }

  function enterAsGuest() {
    api('/api/guest', { method: 'POST' })
      .then(function (data) {
        if (data && data.ok) {
          enterApp(data);
        } else {
          showToast((data && data.error) || '游客登录失败', true);
        }
      })
      .catch(function () { showToast('网络错误，无法进入', true); });
  }

  function submitAuth(event) {
    event.preventDefault();
    var username = (el['in-username'].value || '').trim();
    var password = el['in-password'].value || '';
    if (!username || !password) {
      showAuthError('请输入用户名和密码');
      return;
    }
    var endpoint = authMode === 'login' ? '/api/login' : '/api/register';
    showAuthError('');
    el['auth-submit'].disabled = true;

    api(endpoint, { method: 'POST', body: { username: username, password: password } })
      .then(function (data) {
        el['auth-submit'].disabled = false;
        if (data && data.ok) {
          el['in-password'].value = '';
          enterApp(data);
        } else {
          showAuthError((data && data.error) || '操作失败');
        }
      })
      .catch(function () {
        el['auth-submit'].disabled = false;
        showAuthError('网络错误，请稍后重试');
      });
  }

  function logout() {
    stopPoll();
    stopTransferPoll();
    state.pc = {};
    state.transferSessions = {};
    state.pendingIce = {};
    api('/api/logout', { method: 'POST' }).catch(function () {}).then(function () {
      state.me = null;
      state.myId = 0;
      state.friends = [];
      state.requests = [];
      state.after = 0;
      state.currentRoom = '#world';
      state.currentPeerId = null;
      state.connectedLast = undefined;
      state.reqSig = null;
      state.friendSig = null;
      el.messages.textContent = '';
      if (el.modal) el.modal.classList.add('hidden');
      if (el.sidebar) el.sidebar.classList.remove('open');
      showWelcome();
    });
  }

  /* --------------------------------------------------------------- 侧栏 */

  function closeSidebarOnNarrow() {
    if (window.innerWidth <= 760 && el.sidebar) el.sidebar.classList.remove('open');
  }

  /* --------------------------------------------------------------- 绑定 */

  function bindEvents() {
    /* 首屏 */
    el['btn-guest'].addEventListener('click', enterAsGuest);
    el['btn-open-login'].addEventListener('click', function () { openModal('login'); });
    el['btn-open-register'].addEventListener('click', function () { openModal('register'); });

    /* 弹窗 */
    el['tab-login'].addEventListener('click', function () { setAuthMode('login'); });
    el['tab-register'].addEventListener('click', function () { setAuthMode('register'); });
    el['modal-close'].addEventListener('click', closeModal);
    el.modal.addEventListener('click', function (e) {
      if (e.target === el.modal) closeModal();
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') closeModal();
    });
    el['auth-form'].addEventListener('submit', submitAuth);

    /* 顶栏 */
    el['btn-logout'].addEventListener('click', logout);
    el['btn-sidebar-toggle'].addEventListener('click', function () {
      el.sidebar.classList.toggle('open');
    });

    /* 身份码 */
    el['btn-edit-handle'].addEventListener('click', function () {
      el['handle-editor'].classList.remove('hidden');
      el['in-handle'].value = (state.me && state.me.handle) || '';
      el['in-handle'].focus();
    });
    el['btn-handle-cancel'].addEventListener('click', function () {
      el['handle-editor'].classList.add('hidden');
    });
    el['btn-handle-save'].addEventListener('click', saveHandle);
    el['in-handle'].addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); saveHandle(); }
    });

    /* 世界聊天 */
    el['btn-world'].addEventListener('click', goWorld);

    /* 添加好友 */
    el['btn-add-friend'].addEventListener('click', addFriend);
    el['in-add-friend'].addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); addFriend(); }
    });

    /* 发送消息 */
    el.composer.addEventListener('submit', function (e) {
      e.preventDefault();
      sendMessage();
    });

    /* 发送文件（WebRTC 点对点） */
    el['btn-attach'].addEventListener('click', function () {
      if (state.mode === 'guest') { showToast('游客不能发送文件', true); return; }
      if (!state.currentPeerId) { showToast('请先选择一位好友进入私聊', true); return; }
      el['file-input'].click();
    });
    el['file-input'].addEventListener('change', function (e) {
      var file = e.target.files && e.target.files[0];
      e.target.value = ''; /* 允许再次选择同一文件 */
      if (file) sendFile(file);
    });

    /* 图片/视频全屏预览：点击空白处关闭 */
    el.lightbox.addEventListener('click', function () {
      el.lightbox.classList.add('hidden');
      el.lightbox.textContent = '';
    });
  }

  /* --------------------------------------------------------------- 启动 */

  function init() {
    cacheDom();
    bindEvents();
    setConnected(false);

    /* 尝试恢复已有会话（无 Cookie 时后端返回 401，属于正常情况） */
    api('/api/me').then(function (data) {
      if (data && data.ok) {
        enterApp(data);
      } else {
        showWelcome();
      }
    }).catch(function () {
      showWelcome();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
