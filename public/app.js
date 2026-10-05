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

  /* 长轮询节奏：服务器在没有新数据时会挂起请求（默认最多 25 秒，
   * 见服务端 CHAT_POLL_WAIT_MS），有新数据立即返回。因此客户端不需要
   * 每秒空轮询，只需在每次返回后尽快发起下一次即可。 */
  var POLL_GAP = 300;      /* 一次长轮询返回后到下一次请求的间隔（毫秒） */
  var POLL_RETRY = 1000;   /* 请求出错后的重试间隔（毫秒） */
  var TRANSFER_GAP = 200;  /* 传输信令长轮询返回后到下一次请求的间隔（毫秒） */

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
    if (options.signal) init.signal = options.signal;   /* 长轮询可被 AbortController 取消 */
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
    view: 'chat',           /* 右侧主区显示哪个视图：'chat' | 'dashboard' */
    after: 0,
    friends: [],
    requests: [],
    pollTimer: null,
    polling: false,
    pollAbort: null,        /* 在途长轮询的 AbortController（切换房间 / 退出时取消） */
    friendsRev: 0,          /* 好友关系版本号：/api/poll 返回变化时刷新好友列表 */
    toastTimer: null,
    /* 文件传输（WebRTC 信令与数据通道） */
    pc: {},                /* peerId -> RTCPeerConnection */
    transferSessions: {},  /* 'send:<peerId>' / 'recv:<peerId>' -> 会话 */
    pendingIce: {},        /* peerId -> 待 addIceCandidate 的候选队列 */
    transferPollTimer: null,
    transferPolling: false,
    /* 渲染签名：轮询响应会频繁调用 render*，若每次都重建 DOM，按钮会在点击瞬间
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
      'room-name', 'conn-status', 'mode-tag', 'btn-logout', 'btn-theme',
      'btn-sidebar-toggle', 'sidebar',
      'my-uid', 'my-handle', 'btn-edit-handle', 'handle-editor', 'in-handle',
      'btn-handle-save', 'btn-handle-cancel',
      'btn-world', 'btn-dashboard', 'req-badge', 'request-list', 'friend-list',
      'add-friend-card', 'in-add-friend', 'btn-add-friend',
      'messages', 'messages-empty', 'composer', 'in-message',
      'btn-attach', 'file-input',
      'view-dashboard', 'dash-path', 'dash-list',
      'pw-form', 'in-old-password', 'in-new-password', 'in-new-password2',
      'pw-error', 'pw-submit',
      'field-password2', 'in-password2'
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

  /* --------------------------------------------------------------- 主题 */

  /**
   * 应用主题：在 <html> 上写 data-theme，CSS 变量整套跟着切换。
   * 首帧由 index.html 里的内联脚本先行定好，这里只负责后续同步与持久化。
   */
  function applyTheme(theme) {
    var dark = theme === 'dark';
    document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
    if (el['btn-theme']) {
      el['btn-theme'].textContent = dark ? '☀️' : '🌙';
      el['btn-theme'].setAttribute('title', dark ? '切换到浅色主题' : '切换到深色主题');
    }
  }

  /** 主题偏好优先级：localStorage > 系统偏好 > 浅色 */
  function initTheme() {
    var saved = null;
    try { saved = window.localStorage.getItem('chat-theme'); } catch (e) { saved = null; }
    if (saved !== 'dark' && saved !== 'light') {
      saved = (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches)
        ? 'dark' : 'light';
    }
    applyTheme(saved);
  }

  /** 深浅切换并记忆；隐私模式下 localStorage 抛错时降级为仅本次生效 */
  function toggleTheme() {
    var next = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
    applyTheme(next);
    try { window.localStorage.setItem('chat-theme', next); } catch (e) { /* 忽略 */ }
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
    if (el['in-password2']) el['in-password2'].value = '';
  }

  function setAuthMode(mode) {
    authMode = mode === 'register' ? 'register' : 'login';
    var isLogin = authMode === 'login';
    el['tab-login'].classList.toggle('active', isLogin);
    el['tab-register'].classList.toggle('active', !isLogin);
    el['auth-submit'].textContent = isLogin ? '登录' : '注册';
    /* 确认密码只在注册时出现；切回登录时清空，避免残留内容被误提交 */
    if (el['field-password2']) el['field-password2'].classList.toggle('hidden', isLogin);
    if (el['in-password2']) el['in-password2'].value = '';
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

    /* 游客：隐藏修改身份码、添加好友与仪表盘 */
    el['btn-edit-handle'].classList.toggle('hidden', isGuest);
    el['add-friend-card'].classList.toggle('hidden', isGuest);
    el['btn-attach'].classList.toggle('hidden', isGuest);
    el['btn-dashboard'].classList.toggle('hidden', isGuest);
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
    /* 好友项带 active 高亮，所以签名里要带上当前私聊对象与当前视图 */
    var sig = signatureOf(state.mode, state.friends, state.view + '|' + state.currentPeerId);
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
      if (state.view === 'chat' && String(friend.id) === String(state.currentPeerId)) li.classList.add('active');

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

  /* 侧栏导航高亮：世界聊天与仪表盘两个入口互斥，统一在这里同步。
   * 之前 setRoom() 只负责点亮「世界聊天」、从不熄灭「仪表盘」，
   * 导致从仪表盘切回世界聊天后两个按钮同时高亮。 */
  function syncNavActive() {
    if (el['btn-world']) {
      el['btn-world'].classList.toggle('active',
        state.view === 'chat' && state.currentRoom === '#world');
    }
    if (el['btn-dashboard']) {
      el['btn-dashboard'].classList.toggle('active', state.view === 'dashboard');
    }
  }

  function setRoom(room, displayName, peerId) {
    state.currentRoom = room;
    state.currentPeerId = peerId === undefined ? null : peerId;
    state.after = 0;
    state.view = 'chat';                 /* 切回聊天视图（会关掉仪表盘） */
    hideDashboard();
    el['room-name'].textContent = displayName;
    el.messages.textContent = '';
    el['messages-empty'].classList.remove('hidden');
    syncNavActive();                     /* 两个导航项一次同步到位 */
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

  /* --------------------------------------------------------------- 仪表盘 */

  /* 关掉仪表盘，恢复聊天区（消息列表 + 输入条） */
  function hideDashboard() {
    if (el['view-dashboard']) el['view-dashboard'].classList.add('hidden');
    if (el.messages) el.messages.classList.remove('hidden');
    if (el.composer) el.composer.classList.remove('hidden');
    /* 空状态要按「当前到底有没有消息」决定，不能无条件显示 */
    if (el['messages-empty'] && el.messages && el.messages.children.length === 0) {
      el['messages-empty'].classList.remove('hidden');
    }
  }

  function showPwError(message) {
    if (!el['pw-error']) return;
    if (message) {
      el['pw-error'].textContent = message;
      el['pw-error'].classList.remove('hidden');
    } else {
      el['pw-error'].textContent = '';
      el['pw-error'].classList.add('hidden');
    }
  }

  function showDashboard() {
    if (!state.me) return;
    if (state.mode === 'guest') {
      showToast('游客没有仪表盘，请注册后使用', true);
      return;
    }
    state.view = 'dashboard';
    el.messages.classList.add('hidden');
    el['messages-empty'].classList.add('hidden');
    el.composer.classList.add('hidden');
    el['view-dashboard'].classList.remove('hidden');
    syncNavActive();                 /* 世界聊天熄灭、仪表盘点亮 */
    renderFriends();                 /* 取消好友高亮 */
    closeSidebarOnNarrow();
    showPwError('');
    loadDashboard();
  }

  function loadDashboard() {
    api('/api/dashboard').then(function (data) {
      if (data && data.ok) {
        renderDashboard(data);
      } else {
        renderDashboard(null);
        showToast((data && data.error) || '仪表盘加载失败', true);
      }
    }).catch(function () {
      renderDashboard(null);
      showToast('网络错误，仪表盘加载失败', true);
    });
  }

  /* 逐行渲染个人数据；data 为 /api/dashboard 的完整响应 */
  function renderDashboard(data) {
    var list = el['dash-list'];
    var pathEl = el['dash-path'];
    var d = (data && data.dashboard) ? data.dashboard : null;

    if (pathEl) {
      if (data && data.path) {
        pathEl.textContent = '数据文件：' + data.path +
          (data.file_written === false ? '（写入失败，请检查目录权限）' : '');
      } else {
        pathEl.textContent = '—';
      }
    }
    if (!list) return;

    list.textContent = '';
    if (!d) {
      var p = document.createElement('p');
      p.className = 'dash-empty';
      p.textContent = '暂无数据';
      list.appendChild(p);
      return;
    }

    /* [标签, 值, 是否等宽字体] */
    var rows = [
      ['用户名', d.username, true],
      ['数字 id', d.id, true],
      ['标识码', d.uid, true],
      ['身份码', d.handle, true],
      ['注册时间', d.created_at, false],
      ['好友数', d.friend_count, false],
      ['发言数', d.message_count, false],
      ['数据更新', d.updated_at, false]
    ];
    rows.forEach(function (row) {
      var li = document.createElement('li');
      li.className = 'dash-row';

      var k = document.createElement('span');
      k.className = 'dash-key';
      k.textContent = row[0];

      var v = document.createElement('span');
      v.className = 'dash-val' + (row[2] ? ' mono' : '');
      /* 一律用 textContent：服务端内容不需要、也不允许被当成 HTML 解析 */
      v.textContent = (row[1] === undefined || row[1] === null) ? '—' : String(row[1]);

      li.appendChild(k);
      li.appendChild(v);
      list.appendChild(li);
    });
  }

  function submitPasswordChange(event) {
    event.preventDefault();
    var oldPw = (el['in-old-password'] && el['in-old-password'].value) || '';
    var newPw = (el['in-new-password'] && el['in-new-password'].value) || '';
    var newPw2 = (el['in-new-password2'] && el['in-new-password2'].value) || '';

    if (!oldPw || !newPw) { showPwError('请填写当前密码与新密码'); return; }
    if (newPw !== newPw2) { showPwError('两次输入的新密码不一致'); return; }
    if (newPw.length < 6 || newPw.length > 64) { showPwError('新密码长度需为 6-64 位'); return; }
    if (newPw === oldPw) { showPwError('新密码不能与当前密码相同'); return; }

    showPwError('');
    el['pw-submit'].disabled = true;
    api('/api/password', {
      method: 'POST',
      body: { old_password: oldPw, new_password: newPw, new_password2: newPw2 }
    }).then(function (data) {
      el['pw-submit'].disabled = false;
      if (data && data.ok) {
        el['in-old-password'].value = '';
        el['in-new-password'].value = '';
        el['in-new-password2'].value = '';
        showToast('密码已修改，其它设备已下线', false);
      } else {
        showPwError((data && data.error) || '修改失败');
      }
    }).catch(function () {
      el['pw-submit'].disabled = false;
      showPwError('网络错误，请稍后重试');
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

  function sendFileData(channel, buffer, meta, card) {
    channel.send(JSON.stringify({
      type: 'meta', name: meta.name, size: meta.size,
      mime: meta.mime, sha256: meta.sha256, thumb: meta.thumb
    }));

    var size = buffer.byteLength;

    /* 分片大小：改成发送 ArrayBuffer，并严格限制在数据通道
     * maxMessageSize 以内（留 16 字节余量）。
     * 之前的实现用 file.slice() 发 Blob + 固定 64KiB/16KiB：
     *   1) Blob 经 send() 异步读取，尺寸触碰 maxMessageSize 时会抛异常或
     *      被浏览器静默丢弃 → 接收方拿到 0 字节 / 文件损坏；
     *   2) 固定 16KiB 仍可能超过某些实现协商出的 maxMessageSize。
     * 这里按通道实际上限动态收窄，并用 ArrayBuffer 精确控制每一片的字节数。 */
    var CHUNK = 16 * 1024;
    if (channel.maxMessageSize && channel.maxMessageSize > 0) {
      CHUNK = Math.min(CHUNK, channel.maxMessageSize - 16);
    }
    if (CHUNK < 1) CHUNK = 1;

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
      while (offset < size && channel.bufferedAmount < HIGH) {
        var end = Math.min(offset + CHUNK, size);   /* 最后一片精确到文件末尾 */
        try {
          channel.send(buffer.slice(offset, end));
        } catch (e) {
          fail();
          return;
        }
        offset = end;
        if (card) card.setProgress(offset, size);
      }
      if (offset >= size) {
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

    /* 先把文件读成 ArrayBuffer，再交给 sendFileData 分片发送；
     * 与 prepareMeta（哈希 + 缩略图）并行，避免多读一轮文件。 */
    Promise.all([prepareMeta(file), file.arrayBuffer()]).then(function (res) {
      var meta = res[0];
      var buffer = res[1];
      var pc = createPeerConnection(peerId);
      var channel = pc.createDataChannel('file', { ordered: true });
      var card = appendFileCard({
        own: true, name: file.name, size: file.size,
        mime: meta.mime, thumb: meta.thumb, status: 'waiting'
      });
      state.transferSessions['send:' + peerId] = { dir: 'send', channel: channel, card: card };
      channel.onopen = function () { sendFileData(channel, buffer, meta, card); };
      channel.onerror = function () { card.setStatus('传输失败', true); };
      pc.createOffer()
        .then(function (offer) { return pc.setLocalDescription(offer); })
        .then(function () { return postSignal(peerId, 'offer', JSON.stringify(pc.localDescription)); })
        .catch(function () { card.setStatus('传输失败', true); });
    }).catch(function () {
      showToast('读取文件失败，请重试', true);
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
    /* 完整性校验第一道：实际收到的字节数必须与声明大小一致。
     * 之前只判断「是否收到过数据」，一旦有分片丢失（send 失败 / 数据通道丢包），
     * 就会把损坏或 0 字节的文件标记成「已完成」交给人下载。
     * SHA-256 是第二道（仅在 HTTPS / localhost 等安全上下文可用），
     * 这一道在所有环境下都生效。0 字节的空文件同样能正确通过（两者都为 0）。 */
    if (session.received !== meta.size) {
      if (card) card.setStatus('传输失败', true);
      showToast('文件不完整，传输失败', true);
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
    /* 已登出 / 游客：直接停下，不再排下一次
     * （修复：之前登出后这里会留下一个永远空转的定时器） */
    if (!state.me || state.mode === 'guest') return;
    if (state.transferPolling) return;
    state.transferPolling = true;
    api('/api/transfer/poll').then(function (data) {
      state.transferPolling = false;
      if (!state.me || state.mode === 'guest') return;   /* 已登出：不排下一次 */
      if (data && data.ok && Array.isArray(data.signals)) {
        data.signals.forEach(handleSignal);
      }
      scheduleTransferPoll();
    }).catch(function () {
      state.transferPolling = false;
      if (!state.me) return;
      scheduleTransferPoll();
    });
  }

  function scheduleTransferPoll() {
    if (state.transferPollTimer) clearTimeout(state.transferPollTimer);
    state.transferPollTimer = setTimeout(transferPoll, TRANSFER_GAP);
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

  var pollSeq = 0;   /* 每次发起新请求递增；过期响应（含被取消的）直接丢弃 */

  function schedulePoll(delay) {
    if (state.pollTimer) clearTimeout(state.pollTimer);
    state.pollTimer = setTimeout(poll, delay === undefined ? POLL_GAP : delay);
  }

  function pollNow() {
    if (state.pollTimer) { clearTimeout(state.pollTimer); state.pollTimer = null; }
    poll();
  }

  function poll() {
    if (!state.me) return;

    /* 取消上一次仍在途的请求（切房间 / 抢发时由 pollNow 触发）：
     * 服务端对应线程会在最多 CHAT_POLL_WAIT_MS 内自然退出。 */
    if (state.pollAbort) { try { state.pollAbort.abort(); } catch (e) {} }
    var seq = ++pollSeq;
    state.polling = true;

    /* 记下本次请求的房间：请求期间用户可能切换房间，
     * 旧房间的响应一旦写入，既会串台，又会把 after 抬到旧房间的 id 上，
     * 导致新房间里 id 更小的历史消息被永久过滤掉。 */
    var room = state.currentRoom;
    var url = '/api/poll?room=' + encodeURIComponent(room) +
              '&after=' + encodeURIComponent(String(state.after));
    state.pollAbort = (typeof AbortController !== 'undefined') ? new AbortController() : null;
    var signal = state.pollAbort ? state.pollAbort.signal : undefined;

    api(url, { signal: signal }).then(function (data) {
      if (seq !== pollSeq) return;         /* 已被更新的请求取代：静默丢弃 */
      state.polling = false;

      if (room !== state.currentRoom) {
        schedulePoll(POLL_GAP);            /* 已切房间：丢弃这个过期响应 */
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
        /* 好友关系版本号变化（例如对方刚同意了我的申请）→ 立即刷新好友列表，
         * 不需要用户手动刷新页面。 */
        if (typeof data.friends_rev === 'number' &&
            state.mode !== 'guest' && data.friends_rev !== state.friendsRev) {
          state.friendsRev = data.friends_rev;
          loadFriends();
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
      schedulePoll(POLL_GAP);
    }).catch(function (err) {
      if (seq !== pollSeq) return;         /* 过期 / 被取消的请求：静默丢弃 */
      state.polling = false;
      if (err && err.name === 'AbortError') return;   /* 主动取消：pollNow 会立即发起新请求 */
      /* 网络错误静默重试 */
      setConnFromResult(false);
      schedulePoll(POLL_RETRY);
    });
  }

  function stopPoll() {
    if (state.pollTimer) { clearTimeout(state.pollTimer); state.pollTimer = null; }
    if (state.pollAbort) { try { state.pollAbort.abort(); } catch (e) {} state.pollAbort = null; }
    pollSeq++;                             /* 作废在途响应，防止它们再次排程 */
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
    var isRegister = authMode === 'register';
    var body = { username: username, password: password };

    if (!username || !password) {
      showAuthError('请输入用户名和密码');
      return;
    }
    if (isRegister) {
      /* 注册必须两次输入一致：防止按错键盘，注册完就再也登不上 */
      var password2 = (el['in-password2'] && el['in-password2'].value) || '';
      if (password !== password2) {
        showAuthError('两次输入的密码不一致');
        return;
      }
      body.password2 = password2;
    }
    var endpoint = isRegister ? '/api/register' : '/api/login';
    showAuthError('');
    el['auth-submit'].disabled = true;

    api(endpoint, { method: 'POST', body: body })
      .then(function (data) {
        el['auth-submit'].disabled = false;
        if (data && data.ok) {
          el['in-password'].value = '';
          if (el['in-password2']) el['in-password2'].value = '';
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
      state.friendsRev = 0;
      state.view = 'chat';
      state.connectedLast = undefined;
      state.reqSig = null;
      state.friendSig = null;
      el.messages.textContent = '';
      hideDashboard();
      if (el['in-old-password']) el['in-old-password'].value = '';
      if (el['in-new-password']) el['in-new-password'].value = '';
      if (el['in-new-password2']) el['in-new-password2'].value = '';
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
    if (el['btn-theme']) el['btn-theme'].addEventListener('click', toggleTheme);
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

    /* 仪表盘 + 修改密码 */
    el['btn-dashboard'].addEventListener('click', showDashboard);
    el['pw-form'].addEventListener('submit', submitPasswordChange);

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
    initTheme();
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
