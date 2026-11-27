"""Plan verified shared appearance changes without mutating UE during inspection."""
import hashlib
import json


def prepare(job, saved_entry, material_path, geometry_matches):
    """Change the send plan only. Callbacks return verified, existing UE receipts."""
    for suit in job['suits']:
        additions = []
        planned_entries = {item['key']: item for item in suit['entries']}
        planned_materials = {item['key']: item for item in suit['entries'] if item.get('kind') == 'material'}
        for entry in list(suit['entries']):
            if entry.get('kind') != 'skeletalMesh' or not entry.get('materialHandoff'):
                continue
            requested_blockout = entry.get('config', {}).get('materialMode') == 'blockout'
            static_key = entry['key'].replace(':skeletalMesh:', ':staticMesh:')
            planned_static = planned_entries.get(static_key)
            static = saved_entry(static_key, entry['materialHandoff'].get('staticTargetPath'))
            # A new or changed SM must be verified against its current FBX,
            # without importing or saving a production asset during inspection.
            use_planned = bool(planned_static and planned_static.get('sourcePath'))
            if use_planned:
                geometry_source = planned_static
                appearance_source = planned_static
            elif static:
                geometry_source = static
                appearance_source = static
                # Compatibility with callers that supply only a planned receipt.
                if planned_static and static.get('sourceHash') == planned_static.get('sourceHash') and static.get('sourceHash'):
                    use_planned = True
                    appearance_source = planned_static
            else:
                continue
            bindings = dict(appearance_source.get('externalMaterials', {}))
            bindings.update({slot: material_path(key) for slot, key in appearance_source.get('materials', {}).items()})
            bindings = {slot: path for slot, path in bindings.items() if path}
            slots = entry['materialHandoff'].get('slots', [])
            verified_slots = geometry_matches(entry, geometry_source)
            # An explicit Blockout send may reuse verified SM bindings, but an
            # older formal SM receipt must not disable the FBX-colour fallback.
            blockout = requested_blockout or appearance_source.get('config', {}).get('materialMode') == 'blockout'
            original_materials = dict(entry.get('materials', {}))
            original_external = dict(entry.get('externalMaterials', {}))
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
                    material_key = appearance_source.get('materials', {}).get(matches[0]) if use_planned else None
                    if material_key:
                        entry['materials'][slot] = material_key
                    else:
                        entry['externalMaterials'][slot] = bindings[matches[0]]
                elif not blockout and original_materials.get(slot):
                    # 已有明确材质对应不依赖 SM 分区完全相同；这里只禁止猜测借用。
                    entry['materials'][slot] = original_materials[slot]
                elif not blockout and original_external.get(slot):
                    entry['externalMaterials'][slot] = original_external[slot]
                elif blockout:
                    # An extra/renamed slot is not proof of another slot's identity.
                    # Preserve its own FBX colour rather than guessing by array index.
                    planned = planned_materials.get(original_materials.get(slot))
                    if planned and 'blockoutMaterial' in planned.get('config', {}):
                        entry['materials'][slot] = planned['key']
                        continue
                    # Match the suit exporter's string hash for implicit Blockout plans.
                    suffix = hashlib.sha256(slot.encode()).hexdigest()
                    key = entry['key'] + ':blockout:' + suffix[:16] + ':mi'
                    name = 'MI_BO_' + entry['name'] + '_' + suffix[:12]
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
