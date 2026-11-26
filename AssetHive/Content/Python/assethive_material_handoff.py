"""Material adoption occurs on the receiving mesh's source update, never on inspection."""
import copy
import hashlib
import json


def prepare(job, saved_entry, material_path, geometry_matches):
    """Change the send plan only. Callbacks return verified, existing UE receipts."""
    for suit in job['suits']:
        additions = []
        for entry in list(suit['entries']):
            if entry.get('kind') != 'skeletalMesh' or not entry.get('materialHandoff'):
                continue
            saved = saved_entry(entry['key'], entry.get('targetPath'))
            if saved and saved.get('sourceHash') and saved['sourceHash'] == entry.get('sourceHash'):
                # SM changes, global mode changes and LOD-only work cannot rebind an old SK.
                for field in ('materials', 'externalMaterials', 'linkMaterialPaths', 'heroSharedRoles', 'sharedMaterialSlots', 'unresolvedMaterials'):
                    entry.pop(field, None)
                    if field in saved:
                        entry[field] = copy.deepcopy(saved[field])
                for field in ('materialMode', 'blockoutFallback', 'itemMaterialVersion', 'deferMaterials'):
                    entry['config'].pop(field, None)
                    if field in saved.get('config', {}):
                        entry['config'][field] = saved['config'][field]
                entry['adoptStaticMaterials'] = False
                continue
            static_key = entry['key'].replace(':skeletalMesh:', ':staticMesh:')
            static = saved_entry(static_key, entry['materialHandoff'].get('staticTargetPath'))
            if not static:
                continue
            bindings = dict(static.get('externalMaterials', {}))
            bindings.update({slot: material_path(key) for slot, key in static.get('materials', {}).items()})
            bindings = {slot: path for slot, path in bindings.items() if path}
            slots = entry['materialHandoff'].get('slots', [])
            verified_slots = geometry_matches(entry, static)
            blockout = static.get('config', {}).get('materialMode') == 'blockout'
            entry['materials'] = {}
            entry['externalMaterials'] = {}
            entry['linkMaterialPaths'] = {}
            entry['unresolvedMaterials'] = []
            entry['sharedMaterialSlots'] = []
            entry['heroSharedRoles'] = {}
            entry['adoptStaticMaterials'] = True
            entry['config'].pop('materialMode', None)
            if blockout:
                entry['config']['materialMode'] = 'blockout'
            for slot in slots:
                matches = [name for name in bindings if name in verified_slots.get(slot, [])]
                if len(matches) == 1:
                    entry['externalMaterials'][slot] = bindings[matches[0]]
                elif blockout:
                    # An extra/renamed slot is not proof of another slot's identity.
                    # Preserve its own FBX colour rather than guessing by array index.
                    suffix = hashlib.sha256(slot.encode()).hexdigest()[:12]
                    key = entry['key'] + ':blockout:' + suffix + ':mi'
                    name = 'MI_BO_' + entry['name'] + '_' + suffix
                    appearance = entry['materialHandoff'].get('appearances', {}).get(slot) or {'color': [1, 1, 1], 'opacity': 1, 'emissive': [0, 0, 0], 'roughness': 1}
                    material = {'key': key, 'kind': 'material', 'name': name, 'partSlot': entry.get('partSlot', ''),
                                'targetPath': entry['materialHandoff']['blockoutRoot'] + '/' + name, 'textures': {},
                                'sourceHash': hashlib.sha256(json.dumps(appearance, sort_keys=True).encode()).hexdigest(),
                                'config': {'version': entry['config'].get('version'), 'family': entry['config'].get('family'),
                                           'parentPath': '/Game/MaterialLibrary/Character/Common/M_Blockout',
                                           'blockoutMaterial': appearance, 'blockoutInstanceVersion': 1}}
                    material['fingerprint'] = hashlib.sha256(json.dumps(material, sort_keys=True).encode()).hexdigest()
                    additions.append(material)
                    entry['materials'][slot] = key
                else:
                    entry['unresolvedMaterials'].append(slot)
        used = {key for entry in suit['entries'] for key in entry.get('materials', {}).values()}
        suit['entries'] = [entry for entry in suit['entries'] if not (
            entry.get('kind') == 'material' and 'blockoutMaterial' in entry.get('config', {}) and entry['key'] not in used)]
        existing = {entry['key'] for entry in suit['entries']}
        suit['entries'] = [entry for entry in additions if entry['key'] not in existing] + suit['entries']
