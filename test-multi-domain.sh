#!/bin/bash

# Multi-Domain Setup Test Script
# Tests if all applications are accessible via their domains

echo "🧪 Testing Multi-Domain Setup..."

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_success() { echo -e "${GREEN}✅ $1${NC}"; }
print_error() { echo -e "${RED}❌ $1${NC}"; }
print_testing() { echo -e "${YELLOW}🔍 Testing: $1${NC}"; }

# Test Traefik
print_testing "Global Traefik Status"
if docker ps | grep -q traefik-global; then
    print_success "Traefik is running"
else
    print_error "Traefik is not running"
fi

# Test Traefik network
print_testing "Traefik Network"
if docker network ls | grep -q traefik-global; then
    print_success "Traefik network exists"
else
    print_error "Traefik network not found"
fi

# Test containers with Traefik labels
print_testing "Applications with domain routing"
containers=$(docker ps --filter "label=traefik.enable=true" -q)
if [ -z "$containers" ]; then
    print_error "No applications found with Traefik routing"
else
    print_success "Found $(echo $containers | wc -w) applications with domain routing"
    
    # List each application
    for container in $containers; do
        name=$(docker inspect $container | jq -r '.[0].Name' | sed 's/^\///')
        domain=$(docker inspect $container | jq -r '.[0].Config.Labels | to_entries[] | select(.key | contains("traefik.http.routers") and contains(".rule")) | .value' | grep -o 'Host(`[^`]*`)' | sed 's/Host(`\(.*\)`)/\1/' | head -1)
        
        if [ ! -z "$domain" ]; then
            echo "   • $name → $domain"
            
            # Test if domain resolves to this server
            domain_ip=$(dig +short $domain 2>/dev/null | tail -1)
            server_ip=$(hostname -I | awk '{print $1}')
            
            if [ "$domain_ip" = "$server_ip" ]; then
                print_success "  DNS configured correctly"
            else
                print_error "  DNS not pointing to this server ($domain_ip vs $server_ip)"
            fi
        fi
    done
fi

# Test SSL certificates
print_testing "SSL Certificates"
if [ -f "traefik-data/acme.json" ]; then
    cert_count=$(jq '.letsencrypt.Certificates | length' traefik-data/acme.json 2>/dev/null || echo "0")
    if [ "$cert_count" -gt 0 ]; then
        print_success "Found $cert_count SSL certificates"
    else
        print_error "No SSL certificates found"
    fi
else
    print_error "ACME certificates file not found"
fi

# Test Traefik dashboard
print_testing "Traefik Dashboard"
if curl -s -f http://localhost:8080/api/rawdata > /dev/null 2>&1; then
    print_success "Traefik dashboard is accessible"
else
    print_error "Traefik dashboard is not accessible"
fi

echo ""
echo "📊 Summary:"
echo "   • Total containers: $(docker ps | wc -l | awk '{print $1-1}')"
echo "   • Traefik-enabled: $(docker ps --filter 'label=traefik.enable=true' | wc -l | awk '{print $1-1}')"
echo "   • Networks: $(docker network ls | wc -l | awk '{print $1-1}')"
echo "   • Volumes: $(docker volume ls | wc -l | awk '{print $1-1}')"
echo ""
echo "🔧 Useful Commands:"
echo "   • View all apps:    ./quick-deploy.sh list-apps"
echo "   • System status:    ./quick-deploy.sh status"
echo "   • Traefik logs:     docker logs traefik-global -f"
echo "   • Cleanup:          ./quick-deploy.sh cleanup"
echo ""