// Small DOM helpers of the views. Texts are always set with textContent (service names and EPG
// texts come from the server and are never interpreted as HTML).

export function show(element, visible) {
  element.classList.toggle('hidden', !visible);
}

export function line(parent, text, className = '') {
  const div = document.createElement('div');
  div.textContent = text;
  if (className) div.className = className;
  parent.appendChild(div);
  return div;
}

// Replaces the content of `element` by the result of `build(element)`, only while visible.
export function render(element, visible, build) {
  show(element, visible);
  element.replaceChildren();
  if (visible) build(element);
}
