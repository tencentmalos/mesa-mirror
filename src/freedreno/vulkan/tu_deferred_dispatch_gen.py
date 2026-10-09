import argparse
import sys
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--import-path', required=True)
parser.add_argument('--xml', required=True)
parser.add_argument('--out', required=True)
parser.add_argument('--beta', choices=['true', 'false'], required=True)
args = parser.parse_args()
sys.path.insert(0, args.import_path)

from vk_entrypoints import get_entrypoints_from_xml
from vk_cmd_queue_gen import NO_ENQUEUE_COMMANDS

commands = [c for c in get_entrypoints_from_xml([args.xml], args.beta == 'true')
            if c.alias is None and c.name.startswith('Cmd')
            and c.name not in NO_ENQUEUE_COMMANDS]
lines = ['#include "tu_deferred.h"', '#include "tu_cmd_buffer.h"',
         '#include "tu_device.h"', '#include "vk_cmd_enqueue_entrypoints.h"', '']
for c in commands:
    assert c.return_type == 'void'
    if c.guard:
        lines.append('#ifdef ' + c.guard)
    lines += ['static VKAPI_ATTR void VKAPI_CALL',
              'tu_deferred_' + c.name + '(' + c.decl_params() + ')', '{',
              '   VK_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);']
    trigger = None
    if c.name in ['CmdBeginRenderPass', 'CmdBeginRenderPass2']:
        trigger = 'tu_deferred_needs_render_pass(pRenderPassBegin)'
    elif c.name == 'CmdBeginRendering':
        trigger = 'tu_deferred_needs_rendering(pRenderingInfo)'
    elif c.name == 'CmdExecuteCommands':
        trigger = 'true'
    if trigger:
        lines += ['   if (!cmd->deferred_replaying && ' + trigger + ')',
                  '      cmd->deferred_recording = true;']
    lines += ['   if (cmd->deferred_recording) {',
              '      vk_cmd_enqueue_' + c.name + '(' + c.call_params() + ');']
    if c.name in ['CmdBeginRenderPass', 'CmdBeginRenderPass2']:
        lines.append('      tu_deferred_capture_render_pass(cmd, pRenderPassBegin);')
    if c.name == 'CmdBeginRendering':
        lines.append('      tu_deferred_capture_rendering(cmd, pRenderingInfo);')
    lines += ['   } else {',
              '      cmd->device->deferred_dispatch.' + c.name + '(' + c.call_params() + ');',
              '   }', '}']
    if c.guard:
        lines.append('#endif')
lines += ['', 'void', 'tu_init_deferred_dispatch(struct tu_device *device)', '{',
          '   device->deferred_dispatch = device->vk.dispatch_table;']
for c in commands:
    if c.guard:
        lines.append('#ifdef ' + c.guard)
    lines += ['   if (device->deferred_dispatch.' + c.name + ')',
              '      device->vk.dispatch_table.' + c.name + ' = tu_deferred_' + c.name + ';']
    if c.guard:
        lines.append('#endif')
lines += ['}', '']
with Path(args.out).open('w', encoding='utf-8', newline='\n') as output:
    output.write('\n'.join(lines))
