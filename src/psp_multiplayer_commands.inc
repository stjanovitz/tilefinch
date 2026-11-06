/* The browser produces commands, the worker consumes them. These bounded
   mailbox transitions are shared with host publication/interleaving tests. */
static bool multiplayer_enqueue(PspMultiplayerSession *session,
                                const PspMultiplayerCommand *command)
{
    if (session == NULL || command == NULL) return false;
    unsigned write = atomic_load_explicit(&session->command_write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&session->command_read, memory_order_acquire);
    /* The hosted channel has one in-flight SEND and one terminal CLOSE.
       Routine decisions cannot occupy either progress/retirement reserve. */
    unsigned limit = command->kind == PSP_MULTIPLAYER_COMMAND_CLOSE
        ? PSP_MULTIPLAYER_COMMAND_STORAGE
        : command->kind == PSP_MULTIPLAYER_COMMAND_SEND
            ? PSP_MULTIPLAYER_COMMAND_STORAGE - 1u
            : PSP_MULTIPLAYER_COMMAND_STORAGE - 2u;
    if (write - read >= limit) return false;
    session->commands[write % PSP_MULTIPLAYER_COMMAND_STORAGE] = *command;
    atomic_store_explicit(&session->command_write, write + 1u, memory_order_release);
    return true;
}

static void multiplayer_service_commands(PspMultiplayerSession *session)
{
    unsigned read = atomic_load_explicit(&session->command_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&session->command_write, memory_order_acquire);
    for (unsigned count = 0; read != write && count < 4u; count++) {
        PspMultiplayerCommand command =
            session->commands[read % PSP_MULTIPLAYER_COMMAND_STORAGE];
        /* Applying SEND may publish DRAIN immediately. Retire its mailbox
           ownership before waking the producer; use a copy across slot reuse. */
        atomic_store_explicit(&session->command_read, ++read, memory_order_release);
        multiplayer_apply_command(session, &command);
    }
}
