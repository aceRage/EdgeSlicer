
(function () {
  "use strict";

  function applyTheme() {
    const ua = navigator.userAgent || "";
    document.documentElement.classList.toggle("dark", /dark/i.test(ua) && !/light/i.test(ua));
  }

  function T(key, fallback, arg) {
    let text = fallback || key;
    if (arg !== undefined && arg !== null)
      text = String(text).replace("%s", String(arg));
    return text;
  }

  function SendWXMessage(strMsg) {
    if (window.wx && typeof window.wx.postMessage === "function")
      window.wx.postMessage(strMsg);
  }

  const pluginsById = new Map();
  const permissionDrafts = new Map();
  let selectedPluginId = "";
  let pendingPermissionSave = null;
  let renderedPermissionsSignature = null;
  let fullAccessRequestPrefix = "perm";
  let fullAccessRequestNumber = 0;
  let activeDetailTab = "permissions";
  let selectedCapabilityName = "";
  let selectedCapabilityType = "";
  let configPluginId = "";

  function IsPluginLoading(plugin) { return !!plugin && plugin.loading === true; }
  function GetStatus(plugin) {
    if (!plugin) return "";
    if (plugin.loaded || plugin.enabled) return "Activated";
    return "";
  }

  function IsPluginInstalled(plugin) {
    return !!plugin && plugin.installed !== false && !!plugin.plugin_key;
  }

  function tabFromQuery() {
    try {
      const tab = new URLSearchParams(location.search).get("tab");
      return tab === "config" ? "config" : "permissions";
    } catch (err) {
      return "permissions";
    }
  }

  function ActivateDetailTab(tabId) {
    activeDetailTab = tabId === "config" ? "config" : "permissions";
    document.querySelectorAll("[data-tab]").forEach((btn) => {
      btn.setAttribute("aria-selected", btn.dataset.tab === activeDetailTab ? "true" : "false");
    });
    document.querySelectorAll("[data-panel]").forEach((panel) => {
      panel.hidden = panel.dataset.panel !== activeDetailTab;
    });
    RenderDetails();
  }

  function RenderDetails() {
    const plugin = selectedPluginId ? pluginsById.get(selectedPluginId) : null;
    RenderPermissions(plugin);
    RenderConfig(plugin);
  }

  function ApplyInspectorPlugin(plugin, tab) {
    pluginsById.clear();
    if (plugin && plugin.plugin_key) {
      pluginsById.set(plugin.plugin_key, plugin);
      selectedPluginId = plugin.plugin_key;
    } else {
      selectedPluginId = "";
    }
    if (tab === "config" || tab === "permissions")
      ActivateDetailTab(tab);
    else
      RenderDetails();
  }

function SendMessage(command, payload = {}) {
  const message = {
    sequence_id: Math.round(Date.now() / 1000),
    command: command
  };
  Object.keys(payload).forEach((key) => {
    message[key] = payload[key];
  });
  SendWXMessage(JSON.stringify(message));
}


function HandleStudio(value) {
  const payload = (typeof value === "string") ? SafeJsonParse(value) : value;
  if (!payload || typeof payload !== "object")
    return;
  if (payload.command === "inspector_plugin") {
    ApplyInspectorPlugin(payload.data || {}, payload.tab);
  } else if (payload.command === "status_message") {
    ShowStatusMessage(String(payload.message || ""), String(payload.level || "info"));
  } else if (payload.command === "capability_config") {
    ApplyCapabilityConfig(payload);
  } else if (payload.command === "capability_config_saved") {
    ApplyCapabilityConfigSaved(payload);
  } else if (payload.command === "plugin_permissions_result" && pendingPermissionSave &&
             payload.plugin_key === pendingPermissionSave.pluginKey &&
             payload.request_id === pendingPermissionSave.requestId) {
    const request = pendingPermissionSave;
    pendingPermissionSave = null;
    const draft = permissionDrafts.get(request.pluginKey);
    if (payload.success === true && draft && request.operation === "add" && request.draftOriginal !== undefined)
      delete draft.edits[request.category]?.[request.draftOriginal];
    if (payload.success === true && draft && request.operation === "add" && request.draftOriginal === undefined &&
        draft.additions[request.category] === request.value)
      delete draft.additions[request.category];
    if (payload.success === true && draft && (request.operation === "replace" || request.operation === "remove"))
      delete draft.edits[request.category]?.[request.old_value];
    if (draft)
      ApplyLatestPermissionSource(request.pluginKey);
    renderedPermissionsSignature = null;
    RenderDetails();
  }
}

function CommitPermission(pluginKey, mutation, draftOriginal) {
  if (pendingPermissionSave)
    return;
  const requestId = `${fullAccessRequestPrefix}-${++fullAccessRequestNumber}`;
  const active = document.activeElement;
  const input = active?.closest(".permission-form")?.querySelector("input[type='text']") ||
    (active?.id ? active : null);
  const selection = input?.type === "text" ? [input.selectionStart, input.selectionEnd] : null;
  pendingPermissionSave = { pluginKey, requestId, ...mutation, draftOriginal, focusId: input?.id, selection };
  SendMessage("mutate_plugin_permission", { plugin_key: pluginKey, request_id: requestId, ...mutation });
  renderedPermissionsSignature = null;
  RenderDetails();
}

function ShowStatusMessage(message, level) {
  const bar = document.getElementById("statusBar");
  const text = document.getElementById("statusText");
  if (!bar || !text)
    return;

  const normalizedLevel = ["success", "warn", "error", "info"].includes(level) ? level : "info";
  text.textContent = message;
  text.title = message;
  bar.classList.remove("is-empty", "level-success", "level-warn", "level-error", "level-info");
  bar.classList.add(`level-${normalizedLevel}`);
}

function SafeJsonParse(value) {
  try {
    return JSON.parse(value);
  } catch (err) {
    return null;
  }

function PrunePermissionDraftBookkeeping(draft) {
  for (const [category, edits] of Object.entries(draft.edits || {})) {
    const values = Array.isArray(draft.values[category]) ? draft.values[category] : [];
    for (const [original, value] of Object.entries(edits))
      if (!values.includes(original))
        delete edits[original];
    if (Object.keys(edits).length === 0)
      delete draft.edits[category];
  }

  for (const [category, value] of Object.entries(draft.additions || {}))
    if (String(value ?? "").length === 0)
      delete draft.additions[category];
}

// A permission grant can arrive while the tab is open. Merge the latest stored list into the
// rows on screen, but keep every unsaved value the user typed and let the draft win a conflict.
// Edits are keyed by the original source value, so a grant that adds or reorders rows does not
// move the text to another row.
function MergePermissionSource(draft, sourcePermissions) {
  const previousValues = draft.values || {};
  const nextValues = {};
  const nextPending = {};

  for (const [category, entries] of Object.entries(sourcePermissions || {})) {
    if (category === "threading" && typeof entries === "boolean") {
      nextValues[category] = entries;
      continue;
    }
    if (!Array.isArray(entries))
      continue;

    const merged = entries.map(String);
    const previous = Array.isArray(previousValues[category]) ? previousValues[category] : [];
    // Keep the row of an unsaved edit even if the latest stored list dropped its original
    // value. The user can add the edited value as a new grant or discard the edit.
    for (const original of previous) {
      const edited = draft.edits[category]?.[original];
      if (edited !== undefined && !merged.includes(original))
        merged.push(original);
    }

    nextValues[category] = merged;
    nextPending[category] = merged.map((value) => draft.edits[category]?.[value] ?? value);
  }

  // Keep drafts visible but disabled while a category is unavailable.
  for (const [category, previous] of Object.entries(previousValues)) {
    if (category in nextValues || !Array.isArray(previous))
      continue;
    const retained = previous.filter((value) => draft.edits[category]?.[value] !== undefined);
    if (retained.length) {
      nextValues[category] = retained;
      nextPending[category] = retained.map((value) => draft.edits[category][value]);
    }
  }

  draft.values = nextValues;
  draft.pending = nextPending;
  PrunePermissionDraftBookkeeping(draft);
  draft.sourceSignature = JSON.stringify(sourcePermissions);
}

function ApplyLatestPermissionSource(pluginKey) {
  const draft = permissionDrafts.get(pluginKey);
  if (!draft)
    return;
  MergePermissionSource(draft, pluginsById.get(pluginKey)?.permissions ?? null);
}

function RenderPermissions(plugin) {
  const content = document.getElementById("permissionsContent");
  if (!content)
    return;

  if (!plugin) {
    renderedPermissionsSignature = null;
    content.replaceChildren();
    content.textContent = T("select_plugin", "Select a plugin to view its status and details.");
    return;
  }

  const pluginKey = String(plugin.plugin_key || "");
  const sourcePermissions = plugin.permissions && typeof plugin.permissions === "object" ? plugin.permissions : null;
  let draft = permissionDrafts.get(pluginKey);
  if (!draft) {
    draft = { values: {}, pending: {}, additions: {}, edits: {} };
    permissionDrafts.set(pluginKey, draft);
  }
  // While a save is in flight the rows are disabled and the request predates any source change
  // that arrives now. Merge on the result instead, so the pending row cannot flip underneath it.
  if (pendingPermissionSave?.pluginKey !== pluginKey || sourcePermissions === null)
    MergePermissionSource(draft, sourcePermissions);
  const signature = JSON.stringify([pluginKey, plugin.label, plugin.name, plugin.plugin_id, plugin.installed,
    draft.sourceSignature, pendingPermissionSave?.requestId]);
  if (signature === renderedPermissionsSignature)
    return;
  const active = document.activeElement;
  const focusId = content.dataset.pluginKey === pluginKey && content.contains(active) && active.id ? active.id : null;
  const focusOriginal = focusId ? active.dataset.permissionOriginal : undefined;
  const focusCategory = focusId ? active.dataset.permissionCategory : undefined;
  const selection = focusId && active.type === "text" ? [active.selectionStart, active.selectionEnd] : null;
  const scrollPositions = [];
  for (let element = content; element; element = element.parentElement)
    scrollPositions.push([element, element.scrollLeft, element.scrollTop]);
  if (content.dataset.pluginKey === pluginKey) {
    for (const group of content.querySelectorAll("details.permission-group"))
      (draft.openGroups ||= {})[group.dataset.group] = group.open;
  }
  renderedPermissionsSignature = signature;
  content.replaceChildren();
  content.dataset.pluginKey = pluginKey;

  const consoleHeader = document.createElement("header");
  consoleHeader.className = "permission-console-header";
  const headerText = document.createElement("div");
  const pluginName = document.createElement("p");
  pluginName.className = "permission-console-plugin";
  pluginName.textContent = String(plugin.label || plugin.name || plugin.plugin_id || pluginKey);
  headerText.appendChild(pluginName);
  const title = document.createElement("h2");
  title.className = "permission-console-title";
  title.textContent = T("permissions_tab", "Permissions");
  headerText.appendChild(title);
  consoleHeader.appendChild(headerText);
  content.appendChild(consoleHeader);

  const categories = [
    ["fs_read", "permissions_fs_read", "File system read"],
    ["fs_readwrite", "permissions_fs_readwrite", "File system read and write"],
    ["network_http", "permissions_network_http", "HTTP network"],
    ["network_socket", "permissions_network_socket", "Network sockets"],
    ["process", "permissions_process", "Processes"],
    ["threading", "permissions_threading", "Threading"],
    ["events", "permissions_events", "Events"]
  ];
  const categoryGuidance = {
    fs_read: [T("permissions_filesystem_help", "Enter an absolute path on this computer. Add /* at the end to include the folder and everything below it; no other wildcard is accepted. A read/write grant can create files through open(), but it does not cover create or delete events such as os.mkdir and os.remove. Without Full access, each such event outside allowed folders needs its own approval, which is never saved. When a plugin runs in the background, these events are denied without asking."), T("permissions_filesystem_placeholder", "/path/to/directory/*")],
    fs_readwrite: [T("permissions_filesystem_help", "Enter an absolute path on this computer. Add /* at the end to include the folder and everything below it; no other wildcard is accepted. A read/write grant can create files through open(), but it does not cover create or delete events such as os.mkdir and os.remove. Without Full access, each such event outside allowed folders needs its own approval, which is never saved. When a plugin runs in the background, these events are denied without asking."), T("permissions_filesystem_placeholder", "/path/to/directory/*")],
    network_http: [null, "https://example.com"],
    network_socket: [null, "https://example.com"],
    process: [T("permissions_process_help", "Enter the exact value recorded by the audit. It may be a Python representation of the command."), T("permissions_process_placeholder", "Exact audited value")],
    threading: [T("permissions_threading_help_short", "Allow this plugin to create threads."), ""],
    events: [T("permissions_events_help", "Enter a category and event separated by a colon, such as Socket:socket.__new__."), T("permissions_events_placeholder", "Socket:socket.__new__")]
  };
  const placeholder = document.createElement("label");
  placeholder.className = "permission-placeholder";
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.id = "permissionFullAccess";
  checkbox.setAttribute("role", "switch");
  const fullAccessAvailable = typeof plugin.permissions?.full_access === "boolean";
  const fullAccess = plugin.permissions?.full_access === true;
  const installed = IsPluginInstalled(plugin) && !!pluginKey;
  checkbox.checked = fullAccess;
  const fullAccessLocked = !fullAccessAvailable || !installed || !!pendingPermissionSave;
  checkbox.disabled = fullAccessLocked;
  const checkboxInset = document.createElement("span");
  checkboxInset.className = "permission-checkbox-inset";
  const checkboxTrack = document.createElement("span");
  checkboxTrack.className = "permission-switch-track";
  checkboxTrack.setAttribute("aria-hidden", "true");
  checkboxInset.append(checkbox, checkboxTrack);
  const placeholderText = document.createElement("span");
  placeholderText.textContent = T("permissions_full_access_label", "Full access");
  placeholder.append(placeholderText, checkboxInset);
  const overview = document.createElement("div");
  overview.className = "permission-overview";
  content.appendChild(overview);

  const accessCard = document.createElement("div");
  accessCard.className = "permission-access-card";
  if (fullAccess)
    accessCard.classList.add("is-active");
  if (fullAccessLocked)
    accessCard.classList.add("is-disabled");
  accessCard.appendChild(placeholder);
  overview.appendChild(accessCard);
  const fullAccessHelp = document.createElement("p");
  fullAccessHelp.id = "permissionFullAccessHelp";
  fullAccessHelp.className = "permission-full-access-help";
  fullAccessHelp.textContent = T("permissions_full_access_help", "Full access bypasses permission checks. The plugin can read and write most files you can access; modifying startup or application files can let code run outside these checks. Direct Python access to EdgeSlicer's configuration, certificate, install-state and network plug-in paths stays blocked, but subprocesses are not restricted.");
  accessCard.appendChild(fullAccessHelp);
  checkbox.setAttribute("aria-describedby", fullAccessHelp.id);
  if (!fullAccessAvailable) {
    const status = document.createElement("p");
    status.className = "permission-full-access-help";
    status.setAttribute("role", "status");
    status.textContent = T("permissions_full_access_unavailable", "Full access status is unavailable.");
    overview.insertBefore(status, accessCard);
  }

  const editor = document.createElement("fieldset");
  editor.className = "permission-editor";
  editor.disabled = !installed || !fullAccessAvailable || fullAccess || !!pendingPermissionSave;
  checkbox.addEventListener("change", () => {
    const enabled = checkbox.checked;
    checkbox.checked = fullAccess;
    CommitPermission(pluginKey, { operation: "set", category: "full_access", value: enabled });
  });
  if (fullAccess) {
    const notice = document.createElement("p");
    notice.className = "permission-access-notice";
    notice.textContent = T("permissions_full_access_editor_disabled", "Permission list editing is disabled while full access is on.");
    overview.insertBefore(notice, accessCard);
  }
  content.appendChild(editor);

  if (!installed) {
    const notice = document.createElement("p");
    notice.className = "permission-notice";
    notice.textContent = T("permissions_install_to_edit", "Install this plugin to edit its permissions.");
    overview.insertBefore(notice, accessCard);
  }

  const rows = document.createElement("div");
  rows.className = "permission-rows";
  editor.appendChild(rows);
  const groups = [
    { keys: ["fs_read", "fs_readwrite"], title: T("permissions_group_filesystem", "File system") },
    { keys: ["network_http", "network_socket"], title: T("permissions_network_access_title", "Network access"),
      guidance: T("permissions_network_access_guidance", "Enter a full http:// or https:// URL; only its scheme and host are saved, so the grant covers every port, path, and query on that host. A network socket grant also accepts a bare host name or IP address, with or without a port, and covers every port on that host. Other schemes are not accepted. Grants in either list are checked for HTTP and socket access.") },
    { keys: ["process", "threading"], title: T("permissions_group_execution", "Execution") },
    { keys: ["events"], title: T("permissions_group_events", "Audit events") }
  ];
  const groupBodies = new Map();
  groups.forEach((group, index) => {
    const card = document.createElement("details");
    card.className = "permission-group";
    card.dataset.group = group.keys[0];
    card.open = draft.openGroups?.[card.dataset.group] ?? false;
    card.addEventListener("toggle", () => {
      if (card.isConnected && content.dataset.pluginKey === pluginKey)
        (draft.openGroups ||= {})[card.dataset.group] = card.open;
    });
    const header = document.createElement("summary");
    header.className = "permission-group-header";
    const title = document.createElement("span");
    title.className = "permission-group-title";
    title.id = `permission-group-${index}`;
    title.textContent = group.title;
    card.setAttribute("aria-labelledby", title.id);
    header.appendChild(title);
    const chevron = document.createElement("span");
    chevron.className = "permission-group-chevron";
    chevron.setAttribute("aria-hidden", "true");
    header.appendChild(chevron);
    const body = document.createElement("div");
    body.className = "permission-group-body";
    if (group.guidance) {
      const guidance = document.createElement("p");
      guidance.className = "permission-guidance permission-group-guidance";
      guidance.id = "permission-guidance-network";
      guidance.textContent = group.guidance;
      body.appendChild(guidance);
    }
    card.append(header, body);
    rows.appendChild(card);
    group.keys.forEach((key) => groupBodies.set(key, body));
  });
  for (const [key, translation, fallback] of categories) {
    if (key === "fs_read") {
      groupBodies.get(key).appendChild(RenderFilesystemSection(plugin, pluginKey, draft, installed, categoryGuidance.fs_read));
      continue;
    }
    if (key === "fs_readwrite")
      continue;
    const section = document.createElement("section");
    section.className = "permission-section";
    const description = document.createElement("div");
    description.className = "permission-description";
    const controls = document.createElement("div");
    controls.className = "permission-controls";
    section.append(description, controls);
    const heading = document.createElement("h4");
    heading.className = "permission-heading";
    heading.id = `permission-heading-${key}`;
    heading.textContent = T(translation, fallback);
    description.appendChild(heading);
    const guidanceId = key === "network_http" || key === "network_socket"
      ? "permission-guidance-network" : `permission-guidance-${key}`;
    if (categoryGuidance[key][0]) {
      const guidance = document.createElement("p");
      guidance.className = "permission-guidance";
      guidance.id = guidanceId;
      guidance.textContent = categoryGuidance[key][0];
      description.appendChild(guidance);
    }
    const entries = draft.values[key];
    const available = key === "threading" ? typeof entries === "boolean" : Array.isArray(entries);
    if (!available) {
      const unavailable = document.createElement("p");
      unavailable.className = "permission-notice";
      unavailable.textContent = T("permissions_unavailable", "Permissions are unavailable.");
      controls.appendChild(unavailable);
      groupBodies.get(key).appendChild(section);
      continue;
    }

    const categoryEnabled = installed && (key === "threading"
      ? typeof plugin.permissions?.[key] === "boolean"
      : Array.isArray(plugin.permissions?.[key]));
    if (key === "threading") {
      const threadLabel = document.createElement("label");
      threadLabel.className = "permission-thread-toggle";
      const threadCheckbox = document.createElement("input");
      threadCheckbox.type = "checkbox";
      threadCheckbox.setAttribute("role", "switch");
      threadCheckbox.id = "permission-thread-toggle";
      threadCheckbox.checked = entries;
      threadCheckbox.disabled = !categoryEnabled;
      threadCheckbox.setAttribute("aria-labelledby", heading.id);
      threadCheckbox.setAttribute("aria-describedby", guidanceId);
      const threadInset = document.createElement("span");
      threadInset.className = "permission-checkbox-inset";
      const threadTrack = document.createElement("span");
      threadTrack.className = "permission-switch-track";
      threadTrack.setAttribute("aria-hidden", "true");
      threadInset.append(threadCheckbox, threadTrack);
      threadLabel.appendChild(threadInset);
      controls.appendChild(threadLabel);
      threadCheckbox.addEventListener("change", () => {
        const value = threadCheckbox.checked;
        threadCheckbox.checked = entries;
        CommitPermission(pluginKey, { operation: "set", category: key, value });
      });
      groupBodies.get(key).appendChild(section);
      continue;
    }

    const list = document.createElement("ul");
    list.className = "permission-list";
    if (entries.length === 0) {
      const empty = document.createElement("p");
      empty.className = "permission-notice";
      empty.textContent = T("permissions_values_empty", "No values added.");
      controls.appendChild(empty);
    }
    for (let index = 0; index < entries.length; index++) {
      const item = document.createElement("li");
      item.appendChild(PermissionForm(pluginKey, key, heading.textContent, index, categoryEnabled, categoryGuidance[key][1], guidanceId, draft));
      list.appendChild(item);
    }
    controls.appendChild(list);
    controls.appendChild(PermissionForm(pluginKey, key, heading.textContent, null, categoryEnabled, categoryGuidance[key][1], guidanceId, draft));
    groupBodies.get(key).appendChild(section);
  }
  const restored = focusOriginal === undefined ? (focusId && document.getElementById(focusId)) :
    Array.from(content.querySelectorAll("input[data-permission-original]"))
      .find((input) => input.dataset.permissionCategory === focusCategory && input.dataset.permissionOriginal === focusOriginal);
  if (restored && !restored.matches(":disabled")) {
    restored.focus({ preventScroll: true });
    if (selection && restored.type === "text")
      restored.setSelectionRange(selection[0], selection[1]);
  }
  for (const [element, left, top] of scrollPositions) {
    element.scrollLeft = left;
    element.scrollTop = top;
  }
}

function RenderFilesystemSection(plugin, pluginKey, draft, editable, guidanceText) {
  const section = document.createElement("section");
  section.className = "permission-section permission-filesystem";
  const description = document.createElement("div");
  description.className = "permission-description";
  const controls = document.createElement("div");
  controls.className = "permission-controls";
  section.append(description, controls);
  const heading = document.createElement("h4");
  heading.className = "permission-heading";
  heading.textContent = T("permissions_files_folders", "Files and folders");
  const guidance = document.createElement("p");
  guidance.className = "permission-guidance";
  guidance.id = "permission-guidance-filesystem";
  guidance.textContent = guidanceText[0];
  description.append(heading, guidance);

  const types = [
    { key: "fs_read", label: T("permissions_access_read", "Read"), name: T("permissions_filesystem_read_name", "File system: Read"), unavailable: T("permissions_read_unavailable", "Read permissions are unavailable.") },
    { key: "fs_readwrite", label: T("permissions_access_readwrite", "Read and write"), name: T("permissions_filesystem_readwrite_name", "File system: Read and write"), unavailable: T("permissions_readwrite_unavailable", "Read and write permissions are unavailable.") }
  ];
  for (const type of types) {
    const bucket = document.createElement("section");
    bucket.className = "permission-filesystem-bucket";
    const typeHeading = document.createElement("h4");
    typeHeading.className = "permission-heading";
    typeHeading.textContent = type.label;
    bucket.appendChild(typeHeading);
    const entries = draft.values[type.key];
    type.available = Array.isArray(entries) && Array.isArray(plugin.permissions?.[type.key]);
    type.enabled = editable && type.available;
    if (!type.available) {
      const notice = document.createElement("p");
      notice.className = "permission-notice";
      notice.textContent = type.unavailable;
      bucket.appendChild(notice);
    }
    if (Array.isArray(entries)) {
      if (!entries.length && type.available) {
        const empty = document.createElement("p");
        empty.className = "permission-notice";
        empty.textContent = T("permissions_values_empty", "No values added.");
        bucket.appendChild(empty);
      }
      const list = document.createElement("ul");
      list.className = "permission-list";
      for (let index = 0; index < entries.length; index++) {
        const item = document.createElement("li");
        item.appendChild(PermissionForm(pluginKey, type.key, type.name, index, type.enabled, guidanceText[1], guidance.id, draft));
        list.appendChild(item);
      }
      bucket.appendChild(list);
      if (type.available)
        bucket.appendChild(PermissionForm(pluginKey, type.key, type.name, null, type.enabled, guidanceText[1], guidance.id, draft));
    }
    controls.appendChild(bucket);
  }
  return section;
}

function PermissionForm(pluginKey, category, categoryName, index, enabled, placeholder, guidanceId, draft) {
  const isAdd = index === null;
  const oldValue = isAdd ? null : draft.values[category][index];
  const sourceValues = pluginsById.get(pluginKey)?.permissions?.[category];
  const noLongerGranted = !isAdd && Array.isArray(sourceValues) && !sourceValues.includes(oldValue);
  const form = document.createElement("form");
  form.className = "permission-form";
  form.noValidate = true;
  const input = document.createElement("input");
  input.type = "text";
  input.id = `permission-input-${category}-${isAdd ? "add" : index}`;
  if (!isAdd) {
    input.dataset.permissionCategory = category;
    input.dataset.permissionOriginal = oldValue;
  }
  input.value = isAdd ? (draft.additions[category] ?? "") : draft.pending[category][index];
  input.required = true;
  input.disabled = !enabled;
  input.setAttribute("aria-describedby", guidanceId);
  input.setAttribute("aria-label", isAdd
    ? T("permissions_add_value_label", "New value for %s", categoryName)
    : T("permissions_edit_value_label", "Edit value for %s", categoryName));
  if (!isAdd) {
    const valueDescription = document.createElement("span");
    valueDescription.id = `${input.id}-original`;
    valueDescription.className = "permission-edit-status";
    valueDescription.textContent = noLongerGranted
      ? T("permissions_edit_no_longer_granted", "The original value is no longer granted: %s. Add the edited value or discard it.", oldValue)
      : T("permissions_edit_original", "Unsaved edit of: %s", oldValue);
    valueDescription.hidden = !noLongerGranted && input.value === oldValue;
    form.appendChild(valueDescription);
    if (!valueDescription.hidden)
      input.setAttribute("aria-describedby", `${guidanceId} ${valueDescription.id}`);
  }
  input.placeholder = isAdd ? placeholder : "";
  // common.js handles document keydown globally; keep text editing native.
  input.addEventListener("keydown", (event) => event.stopPropagation());
  input.addEventListener("input", () => {
    if (isAdd) {
      if (input.value)
        draft.additions[category] = input.value;
      else
        delete draft.additions[category];
    } else {
      draft.pending[category][index] = input.value;
      if (input.value === oldValue && !noLongerGranted) {
        delete draft.edits[category]?.[oldValue];
        if (draft.edits[category] && Object.keys(draft.edits[category]).length === 0)
          delete draft.edits[category];
      } else {
        (draft.edits[category] ||= {})[oldValue] = input.value;
      }
      const valueDescription = form.querySelector(".permission-edit-status");
      valueDescription.hidden = !noLongerGranted && input.value === oldValue;
      input.setAttribute("aria-describedby", valueDescription.hidden ? guidanceId : `${guidanceId} ${valueDescription.id}`);
    }
    submit.disabled = !enabled || !input.value.trim();
    submit.hidden = !isAdd && !noLongerGranted && input.value === oldValue;
  });
  const submit = document.createElement("button");
  submit.type = "submit";
  submit.disabled = !enabled || !input.value.trim();
  submit.hidden = !isAdd && !noLongerGranted && input.value === oldValue;
  submit.textContent = isAdd || noLongerGranted ? T("permissions_add", "Add") : T("permissions_apply", "Apply");
  submit.setAttribute("aria-label", isAdd || noLongerGranted
    ? T("permissions_add_label", "Add value to %s", categoryName)
    : T("permissions_apply_value_label", "Apply value for %s", categoryName));
  form.appendChild(input);
  form.appendChild(submit);
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    if (!input.value.trim() || (!isAdd && !noLongerGranted && input.value === oldValue))
      return;
    CommitPermission(pluginKey, { operation: isAdd || noLongerGranted ? "add" : "replace", category,
      ...(isAdd || noLongerGranted ? {} : { old_value: oldValue }), value: input.value },
      noLongerGranted ? oldValue : undefined);
  });
  if (!isAdd) {
    const remove = document.createElement("button");
    remove.type = "button";
    remove.disabled = !enabled;
    remove.textContent = noLongerGranted ? T("permissions_discard", "Discard") : T("permissions_remove", "Remove");
    remove.setAttribute("aria-label", noLongerGranted
      ? T("permissions_discard_edit_label", "Discard edit of %s", oldValue)
      : T("permissions_remove_label", "Remove %s", oldValue));
    remove.addEventListener("click", () => {
      if (noLongerGranted) {
        delete draft.edits[category]?.[oldValue];
        ApplyLatestPermissionSource(pluginKey);
        renderedPermissionsSignature = null;
        RenderDetails();
        const next = document.getElementById(`permission-input-${category}-add`);
        if (next && !next.matches(":disabled"))
          next.focus({ preventScroll: true });
      } else {
        CommitPermission(pluginKey, { operation: "remove", category, old_value: oldValue });
      }
    });
    form.appendChild(remove);
  }
  return form;
}

function GetConfigurableCapabilities(plugin) {
  return Array.isArray(plugin?.config_capabilities) ? plugin.config_capabilities : [];
}

function RenderConfig(plugin) {
  const empty = document.getElementById("configEmpty");
  const layout = document.getElementById("configLayout");
  const sidebar = document.getElementById("configSidebar");
  if (!empty || !layout || !sidebar)
    return;

  const capabilities = plugin ? GetConfigurableCapabilities(plugin) : [];
  const pluginKey = String(plugin?.plugin_key || "");

  // A different plugin: drop the selection rather than trusting the name to mean the same here.
  if (pluginKey !== configPluginId) {
    configPluginId = pluginKey;
    selectedCapabilityName = "";
    selectedCapabilityType = "";
    ClearCapabilityConfigView();
  }

  if (!plugin || capabilities.length === 0) {
    // Capabilities only exist once the plugin is activated: say that, rather than claiming it has
    // none to configure.
    empty.textContent = !plugin
      ? T("config_select_plugin", "Select a plugin to configure its capabilities")
      : (IsPluginLoading(plugin)
        ? T("config_loading", "Loading the plugin…")
        : (GetStatus(plugin) === "Activated"
          ? T("config_no_capabilities", "This plugin exposes no capabilities")
          : T("config_activate", "Activate this plugin to configure its capabilities")));
    empty.hidden = false;
    layout.hidden = true;
    sidebar.replaceChildren();
    ClearCapabilityConfigView();
    selectedCapabilityName = "";
    selectedCapabilityType = "";
    return;
  }

  empty.hidden = true;
  layout.hidden = false;

  // Keep the selection across a refresh if the capability is still there, else select the first.
  const stillPresent = capabilities.some((capability) =>
    capability.name === selectedCapabilityName && String(capability.type_key || "") === selectedCapabilityType);
  if (!stillPresent) {
    selectedCapabilityName = String(capabilities[0].name || "");
    selectedCapabilityType = String(capabilities[0].type_key || "");
    ClearCapabilityConfigView();
    RequestCapabilityConfig();
  }

  sidebar.replaceChildren();
  for (const capability of capabilities) {
    const name = String(capability.name || "");
    const typeKey = String(capability.type_key || "");
    const item = document.createElement("button");
    item.type = "button";
    item.className = "config-cap";
    item.dataset.capabilityName = name;
    item.dataset.capabilityType = typeKey;
    item.setAttribute("role", "option");

    const isSelected = name === selectedCapabilityName && typeKey === selectedCapabilityType;
    item.classList.toggle("selected", isSelected);
    item.setAttribute("aria-selected", isSelected ? "true" : "false");

    const label = document.createElement("span");
    label.className = "config-cap-name";
    label.textContent = name;
    item.appendChild(label);

    const type = document.createElement("span");
    type.className = "config-cap-type";
    type.textContent = String(capability.type || "");
    item.appendChild(type);

    sidebar.appendChild(item);
  }
}

function OnConfigSidebarClick(event) {
  const item = event.target.closest(".config-cap");
  if (!item)
    return;

  const name = String(item.dataset.capabilityName || "");
  const typeKey = String(item.dataset.capabilityType || "");
  if (!name || (name === selectedCapabilityName && typeKey === selectedCapabilityType))
    return;

  selectedCapabilityName = name;
  selectedCapabilityType = typeKey;

  // The native reply is async: clear now so the old config cannot appear under the new selection.
  ClearCapabilityConfigView();
  RequestCapabilityConfig();
  RenderConfig(pluginsById.get(selectedPluginId));
}

function RequestCapabilityConfig() {
  if (!selectedPluginId || !selectedCapabilityName)
    return;

  SendMessage("plugin_config_get", {
    plugin_key: selectedPluginId,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType
  });
}

// Empties both editors and the footer, so nothing from the previous capability lingers while the
// next one is in flight.
function ClearCapabilityConfigView() {
  const editor = document.getElementById("configEditor");
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");
  const error = document.getElementById("configError");
  const footer = document.getElementById("configFooter");

  if (editor)
    editor.hidden = true;
  if (custom) {
    custom.hidden = true;
    custom.removeAttribute("srcdoc");
  }
  if (text)
    text.value = "";
  if (error) {
    error.hidden = true;
    error.textContent = "";
  }
  if (footer)
    footer.hidden = true;
  SetConfigValidation("");
}

// Replies are async: one for a capability the user has navigated away from is dropped, never
// rendered into the current view.
function IsCurrentCapability(payload) {
  return String(payload?.plugin_key || "") === selectedPluginId &&
    String(payload?.capability_name || "") === selectedCapabilityName;
}

function ApplyCapabilityConfig(payload) {
  if (!IsCurrentCapability(payload))
    return;

  const editor = document.getElementById("configEditor");
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");
  const error = document.getElementById("configError");

  const message = String(payload?.error || "");
  if (error) {
    error.textContent = message;
    error.hidden = message === "";
  }

  const config = (payload && typeof payload.config === "object" && payload.config !== null) ? payload.config : {};
  const html = String(payload?.custom_html || "");

  // The footer belongs to the JSON editor. A custom UI owns its whole surface, including whatever
  // save/restore controls it wants, and reaches the host through the window.orca bridge.
  const footer = document.getElementById("configFooter");
  if (footer)
    footer.hidden = html !== "";

  if (html) {
    if (custom) {
      custom.hidden = false;
      custom.srcdoc = BuildCustomConfigDocument(html, config, OrcaConfigContext(payload, "global"));
    }
    if (editor)
      editor.hidden = true;
    return;
  }

  // Default editor: any reason a custom UI is unavailable already arrived in payload.error.
  if (custom) {
    custom.hidden = true;
    custom.removeAttribute("srcdoc");
  }
  if (editor)
    editor.hidden = false;
  if (text)
    text.value = JSON.stringify(config, null, 2);
  SetConfigValidation("");
}

function SetConfigValidation(message) {
  const node = document.getElementById("configValidation");
  const save = document.getElementById("configSaveBtn");
  if (node) {
    node.textContent = message;
    node.classList.toggle("invalid", message !== "");
  }
  // Invalid JSON is never saved: Save is the only way to persist. The native side re-validates.
  if (save)
    save.disabled = message !== "";
}

function ValidateConfigText() {
  const text = document.getElementById("configText");
  if (!text)
    return false;

  try {
    JSON.parse(text.value);
    SetConfigValidation("");
    return true;
  } catch (err) {
    SetConfigValidation(String(err?.message || T("invalid_json", "Invalid JSON")));
    return false;
  }
}

function SaveCapabilityConfig() {
  const text = document.getElementById("configText");
  if (!text || !selectedPluginId || !selectedCapabilityName)
    return;
  if (!ValidateConfigText())
    return;

  // Sent as text on purpose: the native side is the authority on validity and parses it itself.
  SendMessage("plugin_config_save", {
    plugin_key: selectedPluginId,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType,
    config: text.value
  });
}

// Writes the capability's own get_default_config() over whatever is stored — the host does not know
// what a plugin considers default. The native side confirms first, then replies as if it were a save.
function RestoreCapabilityConfig() {
  if (!selectedPluginId || !selectedCapabilityName)
    return;

  SendMessage("plugin_config_restore", {
    plugin_key: selectedPluginId,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType
  });
}

function ApplyCapabilityConfigSaved(payload) {
  if (!IsCurrentCapability(payload))
    return;

  const error = document.getElementById("configError");
  const message = String(payload?.error || "");
  if (error) {
    error.textContent = message;
    error.hidden = message === "";
  }
  if (payload?.ok !== true)
    return;

  // Reload from what was persisted, not from what was typed.
  const config = (payload && typeof payload.config === "object" && payload.config !== null) ? payload.config : {};
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");

  if (custom && !custom.hidden && custom.contentWindow)
    custom.contentWindow.postMessage({ __orca: "config", config: config }, "*");
  else if (text)
    text.value = JSON.stringify(config, null, 2);

  SetConfigValidation("");
}

function OnCustomConfigMessage(event) {
  const custom = document.getElementById("configCustom");
  // Only the frame we created, and only while it is actually showing.
  if (!custom || custom.hidden || !custom.contentWindow || event.source !== custom.contentWindow)
    return;

  const data = event.data;
  if (!data || !selectedPluginId || !selectedCapabilityName)
    return;

  if (data.__orca === "save") {
    SendMessage("plugin_config_save", {
      plugin_key: selectedPluginId,
      capability_name: selectedCapabilityName,
      capability_type: selectedCapabilityType,
      config: data.config === undefined ? {} : data.config
    });
    return;
  }

  if (data.__orca === "restore")
    RestoreCapabilityConfig();
}


  window.HandleStudio = HandleStudio;
  document.addEventListener("DOMContentLoaded", () => {
    applyTheme();
    document.querySelectorAll("[data-tab]").forEach((btn) => {
      btn.addEventListener("click", () => ActivateDetailTab(btn.dataset.tab));
    });
    document.getElementById("configSidebar")?.addEventListener("click", OnConfigSidebarClick);
    document.getElementById("configSaveBtn")?.addEventListener("click", SaveCapabilityConfig);
    document.getElementById("configRestoreBtn")?.addEventListener("click", RestoreCapabilityConfig);
    document.getElementById("configText")?.addEventListener("keydown", (event) => event.stopPropagation());
    document.getElementById("configText")?.addEventListener("input", ValidateConfigText);
    document.getElementById("permissionsContent")?.addEventListener("keydown", (event) => {
      if (event.target.closest("input, select, button, summary"))
        event.stopPropagation();
    });
    window.addEventListener("message", OnCustomConfigMessage);
    ActivateDetailTab(tabFromQuery());
    SendMessage("inspector_ready");
  });
})();
