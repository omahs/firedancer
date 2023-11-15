# Firedancer End-to-End Tests

The ansible playbooks here can setup a full Solana/Firedancer cluster and run
various tests against the cluster to simulate real world scenarios.

## Pre-requisites

Passwordless SSH access needs to be setup between the Ansible controller node
and the hosts specified in the inventory.

## Development

Create a `inventory/dev.yml` file similar to `inventory/hosts.yml` and populate
it with host information.  Then you can run with `-i inventory/dev.yml`.  If
you want to use a different ansible user, specify that with `-e "ansible_user=<USER>"` to the
`ansible-playbook` command. For example:

```
ansible-playbook -i inventory/dev.yml playbooks/build.yml -e "ansible_user=<USER>"
```

## Roles

- [x] Build fd binaries
- [x] Build solana binaries
- [x] Distribute binaries
- [ ] Genesis for new cluster
- [ ] Bootstrap fd validator
- [ ] Bootstrap solana validator
- [ ] Join fd validator
- [ ] Join solana validator
- [ ] Teardown fd validator
- [ ] Teardown solana validator
