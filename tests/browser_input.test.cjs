// Run without a game build: node --test tests/browser_input.test.cjs
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function setup() {
  const source = fs.readFileSync(path.join(__dirname, '../web/loader.js'), 'utf8');
  function target() {
    const listeners = new Map();
    return {
      addEventListener(type, fn) {
        if (!listeners.has(type)) listeners.set(type, []);
        listeners.get(type).push(fn);
      },
      emit(type, props = {}) {
        for (const fn of listeners.get(type) || []) fn({ preventDefault() {}, ...props });
      },
    };
  }
  const window = target(), canvas = target();
  const keys = [], buttons = [];
  const M = {
    _input_key_hid: (...args) => keys.push(args),
    _input_mouse_button: (...args) => buttons.push(args),
  };
  const context = vm.createContext({ window, document: {}, $: () => canvas, M });
  // Load the production input section without starting the launcher or requiring WebGPU.
  vm.runInContext(source.slice(source.indexOf('const HID ='), source.indexOf('// Gamepads (')), context);
  vm.runInContext('hookInput(M, false)', context);
  return { window, canvas, keys, buttons };
}

test('losing focus releases all held keys and permits pressing them again', () => {
  const { window, keys } = setup();
  window.emit('keydown', { code: 'ArrowUp' });
  window.emit('keydown', { code: 'KeyS' });
  window.emit('keydown', { code: 'KeyS', repeat: true });
  window.emit('blur');
  window.emit('blur');
  window.emit('keydown', { code: 'KeyS' });
  assert.deepEqual(keys, [[82, 1], [22, 1], [82, 0], [22, 0], [22, 1]]);
});

test('normally released keys are not released again on blur', () => {
  const { window, keys } = setup();
  window.emit('keydown', { code: 'KeyS' });
  window.emit('keyup', { code: 'KeyS' });
  window.emit('keydown', { code: 'Unidentified' });
  window.emit('blur');
  assert.deepEqual(keys, [[22, 1], [22, 0]]);
});

test('losing focus releases held mouse buttons using the game button mapping', () => {
  const { window, canvas, buttons } = setup();
  for (const button of [0, 1, 2]) canvas.emit('mousedown', { button });
  window.emit('mouseup', { button: 1 });
  window.emit('blur');
  window.emit('blur');
  assert.deepEqual(buttons, [[0, 1], [2, 1], [1, 1], [2, 0], [0, 0], [1, 0]]);
});

test('extra mouse buttons do not trigger a left click', () => {
  const { window, canvas, buttons } = setup();
  canvas.emit('mousedown', { button: 3 });
  window.emit('mouseup', { button: 3 });
  window.emit('blur');
  assert.deepEqual(buttons, []);
});
