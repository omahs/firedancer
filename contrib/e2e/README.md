# Firedancer End-to-End Tests

The ansible playbooks here can setup a full Solana/Firedancer cluster and run
various tests against the cluster to simulate real world scenarios.

## Pre-requisites

Passwordless SSH access needs to be setup between the Ansible controller node
and the hosts specified in the inventory.

## Development

Create a `inventory/dev.yml` file similar to `inventory/hosts.yml` and populate
it with host information.  Then you can run with `-i inventory/custom.yml`.  If
you want to use a different ansible user, specify that with `-u <USER>` to the
`ansible-playbook` command. For example:

```
ansible-playbook -i inventory/custom.yml playbooks/build.yml -u <USER>
```

## Roles

- Build fd binaries
- Build solana binaries
- Distribute binaries
- Genesis for new cluster
- Bootstrap fd validator
- Bootstrap solana validator
- Join fd validator
- Join solana validator
- Teardown fd validator
- Teardown solana validator
