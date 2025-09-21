# pip install faker
import csv
import random
from faker import Faker

fake = Faker()

def truncate_string(s, length):
    return str(s)[:length].ljust(length)[:length]

def generate_fake_data(n_users=100, n_products=100, n_orders=20):
    """Generate fake relational data matching the original schema"""

    # Generate Users
    print(f"Generating {n_users} users...")
    users = []
    for i in range(1, n_users + 1):
        users.append({
            'user_id': i,
            'username': truncate_string(fake.user_name(), 16),
            'email': truncate_string(fake.email(), 32),
            'age': random.randint(18, 80),
            'city': truncate_string(fake.city(), 16)
        })

    with open('users.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['user_id', 'username', 'email', 'age', 'city'])
        for user in users:
            writer.writerow([user['user_id'], user['username'], user['email'],
                           user['age'], user['city']])

    # Generate Products
    print(f"Generating {n_products} products...")
    categories = ['electronics', 'clothing', 'food', 'books', 'toys', 'sports', 'home']
    brands = ['TechCorp', 'StyleBrand', 'HomeGoods', 'SportsPro', 'BookWorld']

    products = []
    for i in range(1, n_products + 1):
        products.append({
            'product_id': i,
            'title': truncate_string(fake.catch_phrase(), 32),
            'category': truncate_string(random.choice(categories), 16),
            'price': random.randint(5, 500),
            'stock': random.randint(0, 200),
            'brand': truncate_string(random.choice(brands), 16)
        })

    with open('products.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['product_id', 'title', 'category', 'price', 'stock', 'brand'])
        for product in products:
            writer.writerow([product['product_id'], product['title'], product['category'],
                           product['price'], product['stock'], product['brand']])

    # Generate Orders and Order Items
    print(f"Generating {n_orders} orders...")
    order_id_counter = 1000
    orders = []
    order_items = []
    item_id_counter = 1

    for _ in range(n_orders):
        user_id = random.randint(1, n_users)
        num_items = random.randint(1, 5)

        order_total = 0
        order_quantity = 0

        # Generate items for this order
        for _ in range(num_items):
            product_id = random.randint(1, n_products)
            quantity = random.randint(1, 5)
            price = products[product_id - 1]['price']  # Get actual product price
            item_total = price * quantity

            order_items.append({
                'item_id': item_id_counter,
                'order_id': order_id_counter,
                'product_id': product_id,
                'quantity': quantity,
                'price': price,
                'total': item_total
            })

            order_total += item_total
            order_quantity += quantity
            item_id_counter += 1

        discount = int(order_total * random.uniform(0, 0.2))  # 0-20% discount

        orders.append({
            'order_id': order_id_counter,
            'user_id': user_id,
            'total': order_total,
            'total_quantity': order_quantity,
            'discount': discount
        })

        order_id_counter += 1

    # Write orders
    with open('orders.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['order_id', 'user_id', 'total', 'total_quantity', 'discount'])
        for order in orders:
            writer.writerow([order['order_id'], order['user_id'], order['total'],
                           order['total_quantity'], order['discount']])

    # Write order_items
    with open('order_items.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['item_id', 'order_id', 'product_id', 'quantity', 'price', 'total'])
        for item in order_items:
            writer.writerow([item['item_id'], item['order_id'], item['product_id'],
                           item['quantity'], item['price'], item['total']])

    print(f"\n✅ Data generated successfully!")
    print(f"\n📊 Summary:")
    print(f"  - {len(users)} users")
    print(f"  - {len(products)} products")
    print(f"  - {len(orders)} orders with {len(order_items)} line items")

    print(f"\n🔗 Relationships maintained:")
    print(f"  - All user_ids in orders exist in users")
    print(f"  - All product_ids in order_items exist in products")
    print(f"  - All order_ids in order_items exist in orders")

if __name__ == "__main__":
    import sys

    n_users = 100
    n_products = 100
    n_orders = 20

    if len(sys.argv) > 1:
        n_users = int(sys.argv[1])
    if len(sys.argv) > 2:
        n_products = int(sys.argv[2])
    if len(sys.argv) > 3:
        n_orders = int(sys.argv[3])

    generate_fake_data(n_users, n_products, n_orders)
