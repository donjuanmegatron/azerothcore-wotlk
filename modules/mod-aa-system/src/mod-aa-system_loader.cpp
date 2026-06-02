// mod-aa-system_loader.cpp
// Tells AzerothCore to register the AA system scripts at startup.

void AddSC_mod_aa_system();
void AddSC_aa_combat_modifiers();
void AddSC_aa_archetype();
void AddSC_aa_pet();
void AddSC_aa_class();

void Addmod_aa_systemScripts()
{
    AddSC_mod_aa_system();
    AddSC_aa_combat_modifiers();
    AddSC_aa_archetype();
    AddSC_aa_pet();
    AddSC_aa_class();
}
